/*
 * Copyright (C) 2004 Andrew Beekhof <andrew@beekhof.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include <crm_internal.h>

#include <glib.h>

#include <crm/crm.h>
#include <crm/services.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>

#include <crm/common/util.h>
#include <crm/pengine/rules.h>
#include <crm/pengine/internal.h>
#include <unpack.h>

CRM_TRACE_INIT_DATA(pe_status);

#define set_config_flag(data_set, option, flag) do {			\
	const char *tmp = pe_pref(data_set->config_hash, option);	\
	if(tmp) {							\
	    if(crm_is_true(tmp)) {					\
		set_bit(data_set->flags, flag);			\
	    } else {							\
		clear_bit(data_set->flags, flag);		\
	    }								\
	}								\
    } while(0)

gboolean unpack_rsc_op(resource_t * rsc, node_t * node, xmlNode * xml_op, xmlNode ** last_failure,
                       enum action_fail_response *failed, pe_working_set_t * data_set);
static gboolean determine_remote_online_status(pe_working_set_t * data_set, node_t * this_node);

// Bitmask for warnings we only want to print once
uint32_t pe_wo = 0;

static gboolean
is_dangling_container_remote_node(node_t *node)
{
    /* we are looking for a remote-node that was supposed to be mapped to a
     * container resource, but all traces of that container have disappeared 
     * from both the config and the status section. */
    if (is_remote_node(node) &&
        node->details->remote_rsc &&
        node->details->remote_rsc->container == NULL &&
        is_set(node->details->remote_rsc->flags, pe_rsc_orphan_container_filler)) {
        return TRUE;
    }

    return FALSE;
}


/*!
 * \brief Schedule a fence action for a node
 *
 * \param[in,out] data_set  Current working set of cluster
 * \param[in,out] node      Node to fence
 * \param[in]     reason    Text description of why fencing is needed
 */
void
pe_fence_node(pe_working_set_t * data_set, node_t * node, const char *reason)
{
    CRM_CHECK(node, return);

    /* A guest node is fenced by marking its container as failed */
    if (is_container_remote_node(node)) {
        resource_t *rsc = node->details->remote_rsc->container;

        if (is_set(rsc->flags, pe_rsc_failed) == FALSE) {
            if (!is_set(rsc->flags, pe_rsc_managed)) {
                crm_notice("Not fencing guest node %s "
                           "(otherwise would because %s): "
                           "its guest resource %s is unmanaged",
                           node->details->uname, reason, rsc->id);
            } else {
                crm_warn("Guest node %s will be fenced "
                         "(by recovering its guest resource %s): %s",
                         node->details->uname, rsc->id, reason);

                /* We don't mark the node as unclean because that would prevent the
                 * node from running resources. We want to allow it to run resources
                 * in this transition if the recovery succeeds.
                 */
                node->details->remote_requires_reset = TRUE;
                set_bit(rsc->flags, pe_rsc_failed);
            }
        }

    } else if (is_dangling_container_remote_node(node)) {
        crm_info("Cleaning up dangling connection for guest node %s: "
                 "fencing was already done because %s, "
                 "and guest resource no longer exists",
                 node->details->uname, reason);
        set_bit(node->details->remote_rsc->flags, pe_rsc_failed);

    } else if (is_baremetal_remote_node(node)) {
        resource_t *rsc = node->details->remote_rsc;

        if (rsc && (!is_set(rsc->flags, pe_rsc_managed))) {
            crm_notice("Not fencing remote node %s "
                       "(otherwise would because %s): connection is unmanaged",
                       node->details->uname, reason);
        } else if(node->details->remote_requires_reset == FALSE) {
            node->details->remote_requires_reset = TRUE;
            crm_warn("Remote node %s %s: %s",
                     node->details->uname,
                     pe_can_fence(data_set, node)? "will be fenced" : "is unclean",
                     reason);
        }
        node->details->unclean = TRUE;
        pe_fence_op(node, NULL, TRUE, reason, data_set);

    } else if (node->details->unclean) {
        crm_trace("Cluster node %s %s because %s",
                  node->details->uname,
                  pe_can_fence(data_set, node)? "would also be fenced" : "also is unclean",
                  reason);

    } else {
        crm_warn("Cluster node %s %s: %s",
                 node->details->uname,
                 pe_can_fence(data_set, node)? "will be fenced" : "is unclean",
                 reason);
        node->details->unclean = TRUE;
        pe_fence_op(node, NULL, TRUE, reason, data_set);
    }
}

// @TODO xpaths can't handle templates, rules, or id-refs

// nvpair with provides or requires set to unfencing
#define XPATH_UNFENCING_NVPAIR XML_CIB_TAG_NVPAIR                \
    "[(@" XML_NVPAIR_ATTR_NAME "='" XML_RSC_ATTR_PROVIDES "'"    \
    "or @" XML_NVPAIR_ATTR_NAME "='" XML_RSC_ATTR_REQUIRES "') " \
    "and @" XML_NVPAIR_ATTR_VALUE "='unfencing']"

// unfencing in rsc_defaults or any resource
#define XPATH_ENABLE_UNFENCING \
    "/" XML_TAG_CIB "/" XML_CIB_TAG_CONFIGURATION "/" XML_CIB_TAG_RESOURCES   \
    "//" XML_TAG_META_SETS "/" XPATH_UNFENCING_NVPAIR                                               \
    "|/" XML_TAG_CIB "/" XML_CIB_TAG_CONFIGURATION "/" XML_CIB_TAG_RSCCONFIG  \
    "/" XML_TAG_META_SETS "/" XPATH_UNFENCING_NVPAIR

static
void set_if_xpath(unsigned long long flag, const char *xpath,
                  pe_working_set_t *data_set)
{
    xmlXPathObjectPtr result = NULL;

    if (is_not_set(data_set->flags, flag)) {
        result = xpath_search(data_set->input, xpath);
        if (result && (numXpathResults(result) > 0)) {
            set_bit(data_set->flags, flag);
        }
        freeXpathObject(result);
    }
}

gboolean
unpack_config(xmlNode * config, pe_working_set_t * data_set)
{
    const char *value = NULL;
    GHashTable *config_hash = crm_str_table_new();

    data_set->config_hash = config_hash;

    unpack_instance_attributes(data_set->input, config, XML_CIB_TAG_PROPSET, NULL, config_hash,
                               CIB_OPTIONS_FIRST, FALSE, data_set->now);

    verify_pe_options(data_set->config_hash);

    set_config_flag(data_set, "enable-startup-probes", pe_flag_startup_probes);
    if(is_not_set(data_set->flags, pe_flag_startup_probes)) {
        crm_info("Startup probes: disabled (dangerous)");
    }

    value = pe_pref(data_set->config_hash, XML_ATTR_HAVE_WATCHDOG);
    if (value && crm_is_true(value)) {
        crm_notice("Watchdog will be used via SBD if fencing is required");
        set_bit(data_set->flags, pe_flag_have_stonith_resource);
    }

    /* Set certain flags via xpath here, so they can be used before the relevant
     * configuration sections are unpacked.
     */
    set_if_xpath(pe_flag_enable_unfencing, XPATH_ENABLE_UNFENCING, data_set);

    value = pe_pref(data_set->config_hash, "stonith-timeout");
    data_set->stonith_timeout = crm_get_msec(value);
    crm_debug("STONITH timeout: %d", data_set->stonith_timeout);

    set_config_flag(data_set, "stonith-enabled", pe_flag_stonith_enabled);
    crm_debug("STONITH of failed nodes is %s",
              is_set(data_set->flags, pe_flag_stonith_enabled) ? "enabled" : "disabled");

    data_set->stonith_action = pe_pref(data_set->config_hash, "stonith-action");
    if (!strcmp(data_set->stonith_action, "poweroff")) {
        pe_warn_once(pe_wo_poweroff,
                     "Support for stonith-action of 'poweroff' is deprecated "
                     "and will be removed in a future release (use 'off' instead)");
        data_set->stonith_action = "off";
    }
    crm_trace("STONITH will %s nodes", data_set->stonith_action);

    set_config_flag(data_set, "concurrent-fencing", pe_flag_concurrent_fencing);
    crm_debug("Concurrent fencing is %s",
              is_set(data_set->flags, pe_flag_concurrent_fencing) ? "enabled" : "disabled");

    set_config_flag(data_set, "stop-all-resources", pe_flag_stop_everything);
    crm_debug("Stop all active resources: %s",
              is_set(data_set->flags, pe_flag_stop_everything) ? "true" : "false");

    set_config_flag(data_set, "symmetric-cluster", pe_flag_symmetric_cluster);
    if (is_set(data_set->flags, pe_flag_symmetric_cluster)) {
        crm_debug("Cluster is symmetric" " - resources can run anywhere by default");
    }

    value = pe_pref(data_set->config_hash, "no-quorum-policy");

    if (safe_str_eq(value, "ignore")) {
        data_set->no_quorum_policy = no_quorum_ignore;

    } else if (safe_str_eq(value, "freeze")) {
        data_set->no_quorum_policy = no_quorum_freeze;

    } else if (safe_str_eq(value, "suicide")) {
        if (is_set(data_set->flags, pe_flag_stonith_enabled)) {
            int do_panic = 0;

            crm_element_value_int(data_set->input, XML_ATTR_QUORUM_PANIC,
                                  &do_panic);
            if (do_panic || is_set(data_set->flags, pe_flag_have_quorum)) {
                data_set->no_quorum_policy = no_quorum_suicide;
            } else {
                crm_notice("Resetting no-quorum-policy to 'stop': cluster has never had quorum");
                data_set->no_quorum_policy = no_quorum_stop;
            }
        } else {
            crm_config_err("Resetting no-quorum-policy to 'stop': stonith is not configured");
            data_set->no_quorum_policy = no_quorum_stop;
        }

    } else {
        data_set->no_quorum_policy = no_quorum_stop;
    }

    switch (data_set->no_quorum_policy) {
        case no_quorum_freeze:
            crm_debug("On loss of quorum: Freeze resources");
            break;
        case no_quorum_stop:
            crm_debug("On loss of quorum: Stop ALL resources");
            break;
        case no_quorum_suicide:
            crm_notice("On loss of quorum: Fence all remaining nodes");
            break;
        case no_quorum_ignore:
            crm_notice("On loss of quorum: Ignore");
            break;
    }

    set_config_flag(data_set, "stop-orphan-resources", pe_flag_stop_rsc_orphans);
    crm_trace("Orphan resources are %s",
              is_set(data_set->flags, pe_flag_stop_rsc_orphans) ? "stopped" : "ignored");

    set_config_flag(data_set, "stop-orphan-actions", pe_flag_stop_action_orphans);
    crm_trace("Orphan resource actions are %s",
              is_set(data_set->flags, pe_flag_stop_action_orphans) ? "stopped" : "ignored");

    set_config_flag(data_set, "remove-after-stop", pe_flag_remove_after_stop);
    crm_trace("Stopped resources are removed from the status section: %s",
              is_set(data_set->flags, pe_flag_remove_after_stop) ? "true" : "false");

    set_config_flag(data_set, "maintenance-mode", pe_flag_maintenance_mode);
    crm_trace("Maintenance mode: %s",
              is_set(data_set->flags, pe_flag_maintenance_mode) ? "true" : "false");

    set_config_flag(data_set, "start-failure-is-fatal", pe_flag_start_failure_fatal);
    crm_trace("Start failures are %s",
              is_set(data_set->flags,
                     pe_flag_start_failure_fatal) ? "always fatal" : "handled by failcount");

    if (is_set(data_set->flags, pe_flag_stonith_enabled)) {
        set_config_flag(data_set, "startup-fencing", pe_flag_startup_fencing);
    }
    if (is_set(data_set->flags, pe_flag_startup_fencing)) {
        crm_trace("Unseen nodes will be fenced");
    } else {
        pe_warn_once(pe_wo_blind, "Blind faith: not fencing unseen nodes");
    }

    node_score_red = char2score(pe_pref(data_set->config_hash, "node-health-red"));
    node_score_green = char2score(pe_pref(data_set->config_hash, "node-health-green"));
    node_score_yellow = char2score(pe_pref(data_set->config_hash, "node-health-yellow"));

    crm_debug("Node scores: 'red' = %s, 'yellow' = %s, 'green' = %s",
             pe_pref(data_set->config_hash, "node-health-red"),
             pe_pref(data_set->config_hash, "node-health-yellow"),
             pe_pref(data_set->config_hash, "node-health-green"));

    data_set->placement_strategy = pe_pref(data_set->config_hash, "placement-strategy");
    crm_trace("Placement strategy: %s", data_set->placement_strategy);

    return TRUE;
}

static void
destroy_digest_cache(gpointer ptr)
{
    op_digest_cache_t *data = ptr;

    free_xml(data->params_all);
    free_xml(data->params_secure);
    free_xml(data->params_restart);

    free(data->digest_all_calc);
    free(data->digest_restart_calc);
    free(data->digest_secure_calc);

    free(data);
}

node_t *
pe_create_node(const char *id, const char *uname, const char *type,
               const char *score, pe_working_set_t * data_set)
{
    node_t *new_node = NULL;

    if (pe_find_node(data_set->nodes, uname) != NULL) {
        crm_config_warn("Detected multiple node entries with uname=%s"
                        " - this is rarely intended", uname);
    }

    new_node = calloc(1, sizeof(node_t));
    if (new_node == NULL) {
        return NULL;
    }

    new_node->weight = char2score(score);
    new_node->fixed = FALSE;
    new_node->details = calloc(1, sizeof(struct pe_node_shared_s));

    if (new_node->details == NULL) {
        free(new_node);
        return NULL;
    }

    crm_trace("Creating node for entry %s/%s", uname, id);
    new_node->details->id = id;
    new_node->details->uname = uname;
    new_node->details->online = FALSE;
    new_node->details->shutdown = FALSE;
    new_node->details->rsc_discovery_enabled = TRUE;
    new_node->details->running_rsc = NULL;
    new_node->details->type = node_ping;

    if (safe_str_eq(type, "remote")) {
        new_node->details->type = node_remote;
        set_bit(data_set->flags, pe_flag_have_remote_nodes);
    } else if ((type == NULL) || safe_str_eq(type, "member")) {
        new_node->details->type = node_member;
    }

    new_node->details->attrs = crm_str_table_new();

    if (is_remote_node(new_node)) {
        g_hash_table_insert(new_node->details->attrs, strdup(CRM_ATTR_KIND),
                            strdup("remote"));
    } else {
        g_hash_table_insert(new_node->details->attrs, strdup(CRM_ATTR_KIND),
                            strdup("cluster"));
    }

    new_node->details->utilization = crm_str_table_new();

    new_node->details->digest_cache = g_hash_table_new_full(crm_str_hash,
                                                            g_str_equal, free,
                                                            destroy_digest_cache);

    data_set->nodes = g_list_insert_sorted(data_set->nodes, new_node, sort_node_uname);
    return new_node;
}

bool
remote_id_conflict(const char *remote_name, pe_working_set_t *data) 
{
    bool match = FALSE;
#if 1
    pe_find_resource(data->resources, remote_name);
#else
    if (data->name_check == NULL) {
        data->name_check = g_hash_table_new(crm_str_hash, g_str_equal);
        for (xml_rsc = __xml_first_child(parent); xml_rsc != NULL; xml_rsc = __xml_next_element(xml_rsc)) {
            const char *id = ID(xml_rsc);

            /* avoiding heap allocation here because we know the duration of this hashtable allows us to */
            g_hash_table_insert(data->name_check, (char *) id, (char *) id);
        }
    }
    if (g_hash_table_lookup(data->name_check, remote_name)) {
        match = TRUE;
    }
#endif
    if (match) {
        crm_err("Invalid remote-node name, a resource called '%s' already exists.", remote_name);
        return NULL;
    }

    return match;
}


static const char *
expand_remote_rsc_meta(xmlNode *xml_obj, xmlNode *parent, pe_working_set_t *data)
{
    xmlNode *attr_set = NULL;
    xmlNode *attr = NULL;

    const char *container_id = ID(xml_obj);
    const char *remote_name = NULL;
    const char *remote_server = NULL;
    const char *remote_port = NULL;
    const char *connect_timeout = "60s";
    const char *remote_allow_migrate=NULL;
    const char *container_managed = NULL;

    for (attr_set = __xml_first_child(xml_obj); attr_set != NULL; attr_set = __xml_next_element(attr_set)) {
        if (safe_str_neq((const char *)attr_set->name, XML_TAG_META_SETS)) {
            continue;
        }

        for (attr = __xml_first_child(attr_set); attr != NULL; attr = __xml_next_element(attr)) {
            const char *value = crm_element_value(attr, XML_NVPAIR_ATTR_VALUE);
            const char *name = crm_element_value(attr, XML_NVPAIR_ATTR_NAME);

            if (safe_str_eq(name, XML_RSC_ATTR_REMOTE_NODE)) {
                remote_name = value;
            } else if (safe_str_eq(name, "remote-addr")) {
                remote_server = value;
            } else if (safe_str_eq(name, "remote-port")) {
                remote_port = value;
            } else if (safe_str_eq(name, "remote-connect-timeout")) {
                connect_timeout = value;
            } else if (safe_str_eq(name, "remote-allow-migrate")) {
                remote_allow_migrate=value;
            } else if (safe_str_eq(name, XML_RSC_ATTR_MANAGED)) {
                container_managed = value;
            }
        }
    }

    if (remote_name == NULL) {
        return NULL;
    }

    if (remote_id_conflict(remote_name, data)) {
        return NULL;
    }

    pe_create_remote_xml(parent, remote_name, container_id,
                         remote_allow_migrate, container_managed,
                         connect_timeout, remote_server, remote_port);
    return remote_name;
}

static void
handle_startup_fencing(pe_working_set_t *data_set, node_t *new_node)
{
    if ((new_node->details->type == node_remote) && (new_node->details->remote_rsc == NULL)) {
        /* Ignore fencing for remote nodes that don't have a connection resource
         * associated with them. This happens when remote node entries get left
         * in the nodes section after the connection resource is removed.
         */
        return;
    }

    if (is_set(data_set->flags, pe_flag_startup_fencing)) {
        // All nodes are unclean until we've seen their status entry
        new_node->details->unclean = TRUE;

    } else {
        // Blind faith ...
        new_node->details->unclean = FALSE;
    }

    /* We need to be able to determine if a node's status section
     * exists or not separate from whether the node is unclean. */
    new_node->details->unseen = TRUE;
}

gboolean
unpack_nodes(xmlNode * xml_nodes, pe_working_set_t * data_set)
{
    xmlNode *xml_obj = NULL;
    node_t *new_node = NULL;
    const char *id = NULL;
    const char *uname = NULL;
    const char *type = NULL;
    const char *score = NULL;

    for (xml_obj = __xml_first_child(xml_nodes); xml_obj != NULL; xml_obj = __xml_next_element(xml_obj)) {
        if (crm_str_eq((const char *)xml_obj->name, XML_CIB_TAG_NODE, TRUE)) {
            new_node = NULL;

            id = crm_element_value(xml_obj, XML_ATTR_ID);
            uname = crm_element_value(xml_obj, XML_ATTR_UNAME);
            type = crm_element_value(xml_obj, XML_ATTR_TYPE);
            score = crm_element_value(xml_obj, XML_RULE_ATTR_SCORE);
            crm_trace("Processing node %s/%s", uname, id);

            if (id == NULL) {
                crm_config_err("Must specify id tag in <node>");
                continue;
            }
            new_node = pe_create_node(id, uname, type, score, data_set);

            if (new_node == NULL) {
                return FALSE;
            }

/* 		if(data_set->have_quorum == FALSE */
/* 		   && data_set->no_quorum_policy == no_quorum_stop) { */
/* 			/\* start shutting resources down *\/ */
/* 			new_node->weight = -INFINITY; */
/* 		} */

            handle_startup_fencing(data_set, new_node);

            add_node_attrs(xml_obj, new_node, FALSE, data_set);
            unpack_instance_attributes(data_set->input, xml_obj, XML_TAG_UTILIZATION, NULL,
                                       new_node->details->utilization, NULL, FALSE, data_set->now);

            crm_trace("Done with node %s", crm_element_value(xml_obj, XML_ATTR_UNAME));
        }
    }

    if (data_set->localhost && pe_find_node(data_set->nodes, data_set->localhost) == NULL) {
        crm_info("Creating a fake local node");
        pe_create_node(data_set->localhost, data_set->localhost, NULL, 0,
                       data_set);
    }

    return TRUE;
}

static void
setup_container(resource_t * rsc, pe_working_set_t * data_set)
{
    const char *container_id = NULL;

    if (rsc->children) {
        GListPtr gIter = rsc->children;

        for (; gIter != NULL; gIter = gIter->next) {
            resource_t *child_rsc = (resource_t *) gIter->data;

            setup_container(child_rsc, data_set);
        }
        return;
    }

    container_id = g_hash_table_lookup(rsc->meta, XML_RSC_ATTR_CONTAINER);
    if (container_id && safe_str_neq(container_id, rsc->id)) {
        resource_t *container = pe_find_resource(data_set->resources, container_id);

        if (container) {
            rsc->container = container;
            set_bit(container->flags, pe_rsc_is_container);
            container->fillers = g_list_append(container->fillers, rsc);
            pe_rsc_trace(rsc, "Resource %s's container is %s", rsc->id, container_id);
        } else {
            pe_err("Resource %s: Unknown resource container (%s)", rsc->id, container_id);
        }
    }
}

gboolean
unpack_remote_nodes(xmlNode * xml_resources, pe_working_set_t * data_set)
{
    xmlNode *xml_obj = NULL;

    /* generate remote nodes from resource config before unpacking resources */
    for (xml_obj = __xml_first_child(xml_resources); xml_obj != NULL; xml_obj = __xml_next_element(xml_obj)) {
        const char *new_node_id = NULL;

        /* first check if this is a bare metal remote node. Bare metal remote nodes
         * are defined as a resource primitive only. */
        if (xml_contains_remote_node(xml_obj)) {
            new_node_id = ID(xml_obj);
            /* The "pe_find_node" check is here to make sure we don't iterate over
             * an expanded node that has already been added to the node list. */
            if (new_node_id && pe_find_node(data_set->nodes, new_node_id) == NULL) {
                crm_trace("Found baremetal remote node %s in container resource %s", new_node_id, ID(xml_obj));
                pe_create_node(new_node_id, new_node_id, "remote", NULL,
                               data_set);
            }
            continue;
        }

        /* Now check for guest remote nodes.
         * guest remote nodes are defined within a resource primitive.
         * Example1: a vm resource might be configured as a remote node.
         * Example2: a vm resource might be configured within a group to be a remote node.
         * Note: right now we only support guest remote nodes in as a standalone primitive
         * or a primitive within a group. No cloned primitives can be a guest remote node
         * right now */
        if (crm_str_eq((const char *)xml_obj->name, XML_CIB_TAG_RESOURCE, TRUE)) {
            /* expands a metadata defined remote resource into the xml config
             * as an actual rsc primitive to be unpacked later. */
            new_node_id = expand_remote_rsc_meta(xml_obj, xml_resources, data_set);

            if (new_node_id && pe_find_node(data_set->nodes, new_node_id) == NULL) {
                crm_trace("Found guest remote node %s in container resource %s", new_node_id, ID(xml_obj));
                pe_create_node(new_node_id, new_node_id, "remote", NULL,
                               data_set);
            }
            continue;

        } else if (crm_str_eq((const char *)xml_obj->name, XML_CIB_TAG_GROUP, TRUE)) {
            xmlNode *xml_obj2 = NULL;
            /* search through a group to see if any of the primitive contain a remote node. */
            for (xml_obj2 = __xml_first_child(xml_obj); xml_obj2 != NULL; xml_obj2 = __xml_next_element(xml_obj2)) {

                new_node_id = expand_remote_rsc_meta(xml_obj2, xml_resources, data_set);

                if (new_node_id && pe_find_node(data_set->nodes, new_node_id) == NULL) {
                    crm_trace("Found guest remote node %s in container resource %s which is in group %s", new_node_id, ID(xml_obj2), ID(xml_obj));
                    pe_create_node(new_node_id, new_node_id, "remote", NULL,
                                   data_set);
                }
            }
        }
    }
    return TRUE;
}


/* Call this after all the nodes and resources have been
 * unpacked, but before the status section is read.
 *
 * A remote node's online status is reflected by the state
 * of the remote node's connection resource. We need to link
 * the remote node to this connection resource so we can have
 * easy access to the connection resource during the PE calculations.
 */
static void
link_rsc2remotenode(pe_working_set_t *data_set, resource_t *new_rsc)
{
    node_t *remote_node = NULL;

    if (new_rsc->is_remote_node == FALSE) {
        return;
    }

    if (is_set(data_set->flags, pe_flag_quick_location)) {
        /* remote_nodes and remote_resources are not linked in quick location calculations */
        return;
    }

    print_resource(LOG_TRACE, "Linking remote-node connection resource, ", new_rsc, FALSE);

    remote_node = pe_find_node(data_set->nodes, new_rsc->id);
    CRM_CHECK(remote_node != NULL, return;);

    remote_node->details->remote_rsc = new_rsc;
    /* If this is a baremetal remote-node (no container resource
     * associated with it) then we need to handle startup fencing the same way
     * as cluster nodes. */
    if (new_rsc->container == NULL) {
        handle_startup_fencing(data_set, remote_node);
    } else {
        /* At this point we know if the remote node is a container or baremetal
         * remote node, update the #kind attribute if a container is involved */
        g_hash_table_replace(remote_node->details->attrs, strdup(CRM_ATTR_KIND),
                             strdup("container"));
    }
}

static void
destroy_tag(gpointer data)
{
    tag_t *tag = data;

    if (tag) {
        free(tag->id);
        g_list_free_full(tag->refs, free);
        free(tag);
    }
}

/*!
 * \internal
 * \brief Parse configuration XML for resource information
 *
 * \param[in]     xml_resources  Top of resource configuration XML
 * \param[in,out] data_set       Where to put resource information
 *
 * \return TRUE
 *
 * \note unpack_remote_nodes() MUST be called before this, so that the nodes can
 *       be used when common_unpack() calls resource_location()
 */
gboolean
unpack_resources(xmlNode * xml_resources, pe_working_set_t * data_set)
{
    xmlNode *xml_obj = NULL;
    GListPtr gIter = NULL;

    data_set->template_rsc_sets = g_hash_table_new_full(crm_str_hash,
                                                        g_str_equal, free,
                                                        destroy_tag);

    for (xml_obj = __xml_first_child(xml_resources); xml_obj != NULL; xml_obj = __xml_next_element(xml_obj)) {
        resource_t *new_rsc = NULL;

        if (crm_str_eq((const char *)xml_obj->name, XML_CIB_TAG_RSC_TEMPLATE, TRUE)) {
            const char *template_id = ID(xml_obj);

            if (template_id && g_hash_table_lookup_extended(data_set->template_rsc_sets,
                                                            template_id, NULL, NULL) == FALSE) {
                /* Record the template's ID for the knowledge of its existence anyway. */
                g_hash_table_insert(data_set->template_rsc_sets, strdup(template_id), NULL);
            }
            continue;
        }

        crm_trace("Beginning unpack... <%s id=%s... >", crm_element_name(xml_obj), ID(xml_obj));
        if (common_unpack(xml_obj, &new_rsc, NULL, data_set)) {
            data_set->resources = g_list_append(data_set->resources, new_rsc);
            print_resource(LOG_TRACE, "Added ", new_rsc, FALSE);

        } else {
            crm_config_err("Failed unpacking %s %s",
                           crm_element_name(xml_obj), crm_element_value(xml_obj, XML_ATTR_ID));
            if (new_rsc != NULL && new_rsc->fns != NULL) {
                new_rsc->fns->free(new_rsc);
            }
        }
    }

    for (gIter = data_set->resources; gIter != NULL; gIter = gIter->next) {
        resource_t *rsc = (resource_t *) gIter->data;

        setup_container(rsc, data_set);
        link_rsc2remotenode(data_set, rsc);
    }

    data_set->resources = g_list_sort(data_set->resources, sort_rsc_priority);
    if (is_set(data_set->flags, pe_flag_quick_location)) {
        /* Ignore */

    } else if (is_set(data_set->flags, pe_flag_stonith_enabled)
               && is_set(data_set->flags, pe_flag_have_stonith_resource) == FALSE) {

        crm_config_err("Resource start-up disabled since no STONITH resources have been defined");
        crm_config_err("Either configure some or disable STONITH with the stonith-enabled option");
        crm_config_err("NOTE: Clusters with shared data need STONITH to ensure data integrity");
    }

    return TRUE;
}

gboolean
unpack_tags(xmlNode * xml_tags, pe_working_set_t * data_set)
{
    xmlNode *xml_tag = NULL;

    data_set->tags = g_hash_table_new_full(crm_str_hash, g_str_equal, free,
                                           destroy_tag);

    for (xml_tag = __xml_first_child(xml_tags); xml_tag != NULL; xml_tag = __xml_next_element(xml_tag)) {
        xmlNode *xml_obj_ref = NULL;
        const char *tag_id = ID(xml_tag);

        if (crm_str_eq((const char *)xml_tag->name, XML_CIB_TAG_TAG, TRUE) == FALSE) {
            continue;
        }

        if (tag_id == NULL) {
            crm_config_err("Failed unpacking %s: %s should be specified",
                           crm_element_name(xml_tag), XML_ATTR_ID);
            continue;
        }

        for (xml_obj_ref = __xml_first_child(xml_tag); xml_obj_ref != NULL; xml_obj_ref = __xml_next_element(xml_obj_ref)) {
            const char *obj_ref = ID(xml_obj_ref);

            if (crm_str_eq((const char *)xml_obj_ref->name, XML_CIB_TAG_OBJ_REF, TRUE) == FALSE) {
                continue;
            }

            if (obj_ref == NULL) {
                crm_config_err("Failed unpacking %s for tag %s: %s should be specified",
                               crm_element_name(xml_obj_ref), tag_id, XML_ATTR_ID);
                continue;
            }

            if (add_tag_ref(data_set->tags, tag_id, obj_ref) == FALSE) {
                return FALSE;
            }
        }
    }

    return TRUE;
}

/* The ticket state section:
 * "/cib/status/tickets/ticket_state" */
static gboolean
unpack_ticket_state(xmlNode * xml_ticket, pe_working_set_t * data_set)
{
    const char *ticket_id = NULL;
    const char *granted = NULL;
    const char *last_granted = NULL;
    const char *standby = NULL;
    xmlAttrPtr xIter = NULL;

    ticket_t *ticket = NULL;

    ticket_id = ID(xml_ticket);
    if (ticket_id == NULL || strlen(ticket_id) == 0) {
        return FALSE;
    }

    crm_trace("Processing ticket state for %s", ticket_id);

    ticket = g_hash_table_lookup(data_set->tickets, ticket_id);
    if (ticket == NULL) {
        ticket = ticket_new(ticket_id, data_set);
        if (ticket == NULL) {
            return FALSE;
        }
    }

    for (xIter = xml_ticket->properties; xIter; xIter = xIter->next) {
        const char *prop_name = (const char *)xIter->name;
        const char *prop_value = crm_element_value(xml_ticket, prop_name);

        if (crm_str_eq(prop_name, XML_ATTR_ID, TRUE)) {
            continue;
        }
        g_hash_table_replace(ticket->state, strdup(prop_name), strdup(prop_value));
    }

    granted = g_hash_table_lookup(ticket->state, "granted");
    if (granted && crm_is_true(granted)) {
        ticket->granted = TRUE;
        crm_info("We have ticket '%s'", ticket->id);
    } else {
        ticket->granted = FALSE;
        crm_info("We do not have ticket '%s'", ticket->id);
    }

    last_granted = g_hash_table_lookup(ticket->state, "last-granted");
    if (last_granted) {
        ticket->last_granted = crm_parse_int(last_granted, 0);
    }

    standby = g_hash_table_lookup(ticket->state, "standby");
    if (standby && crm_is_true(standby)) {
        ticket->standby = TRUE;
        if (ticket->granted) {
            crm_info("Granted ticket '%s' is in standby-mode", ticket->id);
        }
    } else {
        ticket->standby = FALSE;
    }

    crm_trace("Done with ticket state for %s", ticket_id);

    return TRUE;
}

static gboolean
unpack_tickets_state(xmlNode * xml_tickets, pe_working_set_t * data_set)
{
    xmlNode *xml_obj = NULL;

    for (xml_obj = __xml_first_child(xml_tickets); xml_obj != NULL; xml_obj = __xml_next_element(xml_obj)) {
        if (crm_str_eq((const char *)xml_obj->name, XML_CIB_TAG_TICKET_STATE, TRUE) == FALSE) {
            continue;
        }
        unpack_ticket_state(xml_obj, data_set);
    }

    return TRUE;
}

static void
unpack_handle_remote_attrs(node_t *this_node, xmlNode *state, pe_working_set_t * data_set) 
{
    const char *resource_discovery_enabled = NULL;
    xmlNode *attrs = NULL;
    resource_t *rsc = NULL;
    const char *shutdown = NULL;

    if (crm_str_eq((const char *)state->name, XML_CIB_TAG_STATE, TRUE) == FALSE) {
        return;
    }

    if ((this_node == NULL) || (is_remote_node(this_node) == FALSE)) {
        return;
    }
    crm_trace("Processing remote node id=%s, uname=%s", this_node->details->id, this_node->details->uname);

    this_node->details->remote_maintenance =
        crm_atoi(crm_element_value(state, XML_NODE_IS_MAINTENANCE), "0");

    rsc = this_node->details->remote_rsc;
    if (this_node->details->remote_requires_reset == FALSE) {
        this_node->details->unclean = FALSE;
        this_node->details->unseen = FALSE;
    }
    attrs = find_xml_node(state, XML_TAG_TRANSIENT_NODEATTRS, FALSE);
    add_node_attrs(attrs, this_node, TRUE, data_set);

    shutdown = pe_node_attribute_raw(this_node, XML_CIB_ATTR_SHUTDOWN);
    if (shutdown != NULL && safe_str_neq("0", shutdown)) {
        crm_info("Node %s is shutting down", this_node->details->uname);
        this_node->details->shutdown = TRUE;
        if (rsc) {
            rsc->next_role = RSC_ROLE_STOPPED;
        }
    }
 
    if (crm_is_true(pe_node_attribute_raw(this_node, "standby"))) {
        crm_info("Node %s is in standby-mode", this_node->details->uname);
        this_node->details->standby = TRUE;
    }

    if (crm_is_true(pe_node_attribute_raw(this_node, "maintenance")) ||
        (rsc && !is_set(rsc->flags, pe_rsc_managed))) {
        crm_info("Node %s is in maintenance-mode", this_node->details->uname);
        this_node->details->maintenance = TRUE;
    }

    resource_discovery_enabled = pe_node_attribute_raw(this_node, XML_NODE_ATTR_RSC_DISCOVERY);
    if (resource_discovery_enabled && !crm_is_true(resource_discovery_enabled)) {
        if (is_baremetal_remote_node(this_node) && is_not_set(data_set->flags, pe_flag_stonith_enabled)) {
            crm_warn("ignoring %s attribute on baremetal remote node %s, disabling resource discovery requires stonith to be enabled.",
                     XML_NODE_ATTR_RSC_DISCOVERY, this_node->details->uname);
        } else {
            /* if we're here, this is either a baremetal node and fencing is enabled,
             * or this is a container node which we don't care if fencing is enabled 
             * or not on. container nodes are 'fenced' by recovering the container resource
             * regardless of whether fencing is enabled. */
            crm_info("Node %s has resource discovery disabled", this_node->details->uname);
            this_node->details->rsc_discovery_enabled = FALSE;
        }
    }
}

static bool
unpack_node_loop(xmlNode * status, bool fence, pe_working_set_t * data_set) 
{
    bool changed = false;
    xmlNode *lrm_rsc = NULL;

    for (xmlNode *state = __xml_first_child(status); state != NULL; state = __xml_next_element(state)) {
        const char *id = NULL;
        const char *uname = NULL;
        node_t *this_node = NULL;
        bool process = FALSE;

        if (crm_str_eq((const char *)state->name, XML_CIB_TAG_STATE, TRUE) == FALSE) {
            continue;
        }

        id = crm_element_value(state, XML_ATTR_ID);
        uname = crm_element_value(state, XML_ATTR_UNAME);
        this_node = pe_find_node_any(data_set->nodes, id, uname);

        if (this_node == NULL) {
            crm_info("Node %s is unknown", id);
            continue;

        } else if (this_node->details->unpacked) {
            crm_info("Node %s is already processed", id);
            continue;

        } else if (is_remote_node(this_node) == FALSE && is_set(data_set->flags, pe_flag_stonith_enabled)) {
            // A redundant test, but preserves the order for regression tests
            process = TRUE;

        } else if (is_remote_node(this_node)) {
            bool check = FALSE;
            resource_t *rsc = this_node->details->remote_rsc;

            if(fence) {
                check = TRUE;

            } else if(rsc == NULL) {
                /* Not ready yet */

            } else if (is_container_remote_node(this_node)
                       && rsc->role == RSC_ROLE_STARTED
                       && rsc->container->role == RSC_ROLE_STARTED) {
                /* Both the connection and the underlying container
                 * need to be known 'up' before we volunterily process
                 * resources inside it
                 */
                check = TRUE;
                crm_trace("Checking node %s/%s/%s status %d/%d/%d", id, rsc->id, rsc->container->id, fence, rsc->role, RSC_ROLE_STARTED);

            } else if (is_container_remote_node(this_node) == FALSE
                       && rsc->role == RSC_ROLE_STARTED) {
                check = TRUE;
                crm_trace("Checking node %s/%s status %d/%d/%d", id, rsc->id, fence, rsc->role, RSC_ROLE_STARTED);
            }

            if (check) {
                determine_remote_online_status(data_set, this_node);
                unpack_handle_remote_attrs(this_node, state, data_set);
                process = TRUE;
            }

        } else if (this_node->details->online) {
            process = TRUE;

        } else if (fence) {
            process = TRUE;
        }

        if(process) {
            crm_trace("Processing lrm resource entries on %shealthy%s node: %s",
                      fence?"un":"", is_remote_node(this_node)?" remote":"",
                      this_node->details->uname);
            changed = TRUE;
            this_node->details->unpacked = TRUE;

            lrm_rsc = find_xml_node(state, XML_CIB_TAG_LRM, FALSE);
            lrm_rsc = find_xml_node(lrm_rsc, XML_LRM_TAG_RESOURCES, FALSE);
            unpack_lrm_resources(this_node, lrm_rsc, data_set);
        }
    }
    return changed;
}

/* remove nodes that are down, stopping */
/* create +ve rsc_to_node constraints between resources and the nodes they are running on */
/* anything else? */
gboolean
unpack_status(xmlNode * status, pe_working_set_t * data_set)
{
    const char *id = NULL;
    const char *uname = NULL;

    xmlNode *state = NULL;
    node_t *this_node = NULL;

    crm_trace("Beginning unpack");

    if (data_set->tickets == NULL) {
        data_set->tickets = g_hash_table_new_full(crm_str_hash, g_str_equal,
                                                  free, destroy_ticket);
    }

    for (state = __xml_first_child(status); state != NULL; state = __xml_next_element(state)) {
        if (crm_str_eq((const char *)state->name, XML_CIB_TAG_TICKETS, TRUE)) {
            unpack_tickets_state((xmlNode *) state, data_set);

        } else if (crm_str_eq((const char *)state->name, XML_CIB_TAG_STATE, TRUE)) {
            xmlNode *attrs = NULL;
            const char *resource_discovery_enabled = NULL;

            id = crm_element_value(state, XML_ATTR_ID);
            uname = crm_element_value(state, XML_ATTR_UNAME);
            this_node = pe_find_node_any(data_set->nodes, id, uname);

            if (uname == NULL) {
                /* error */
                continue;

            } else if (this_node == NULL) {
                crm_config_warn("Node %s in status section no longer exists", uname);
                continue;

            } else if (is_remote_node(this_node)) {
                /* online state for remote nodes is determined by the
                 * rsc state after all the unpacking is done. we do however
                 * need to mark whether or not the node has been fenced as this plays
                 * a role during unpacking cluster node resource state */
                this_node->details->remote_was_fenced = 
                    crm_atoi(crm_element_value(state, XML_NODE_IS_FENCED), "0");
                continue;
            }

            crm_trace("Processing node id=%s, uname=%s", id, uname);

            /* Mark the node as provisionally clean
             * - at least we have seen it in the current cluster's lifetime
             */
            this_node->details->unclean = FALSE;
            this_node->details->unseen = FALSE;
            attrs = find_xml_node(state, XML_TAG_TRANSIENT_NODEATTRS, FALSE);
            add_node_attrs(attrs, this_node, TRUE, data_set);

            if (crm_is_true(pe_node_attribute_raw(this_node, "standby"))) {
                crm_info("Node %s is in standby-mode", this_node->details->uname);
                this_node->details->standby = TRUE;
            }

            if (crm_is_true(pe_node_attribute_raw(this_node, "maintenance"))) {
                crm_info("Node %s is in maintenance-mode", this_node->details->uname);
                this_node->details->maintenance = TRUE;
            }

            resource_discovery_enabled = pe_node_attribute_raw(this_node, XML_NODE_ATTR_RSC_DISCOVERY);
            if (resource_discovery_enabled && !crm_is_true(resource_discovery_enabled)) {
                crm_warn("ignoring %s attribute on node %s, disabling resource discovery is not allowed on cluster nodes",
                    XML_NODE_ATTR_RSC_DISCOVERY, this_node->details->uname);
            }

            crm_trace("determining node state");
            determine_online_status(state, this_node, data_set);

            if (is_not_set(data_set->flags, pe_flag_have_quorum)
                && this_node->details->online
                && (data_set->no_quorum_policy == no_quorum_suicide)) {
                /* Everything else should flow from this automatically
                 * At least until the PE becomes able to migrate off healthy resources
                 */
                pe_fence_node(data_set, this_node, "cluster does not have quorum");
            }
        }
    }


    while(unpack_node_loop(status, FALSE, data_set)) {
        crm_trace("Start another loop");
    }

    // Now catch any nodes we didn't see
    unpack_node_loop(status, is_set(data_set->flags, pe_flag_stonith_enabled), data_set);

    for (GListPtr gIter = data_set->nodes; gIter != NULL; gIter = gIter->next) {
        node_t *this_node = gIter->data;

        if (this_node == NULL) {
            continue;
        } else if(is_remote_node(this_node) == FALSE) {
            continue;
        } else if(this_node->details->unpacked) {
            continue;
        }
        determine_remote_online_status(data_set, this_node);
    }

    return TRUE;
}

static gboolean
determine_online_status_no_fencing(pe_working_set_t * data_set, xmlNode * node_state,
                                   node_t * this_node)
{
    gboolean online = FALSE;
    const char *join = crm_element_value(node_state, XML_NODE_JOIN_STATE);
    const char *is_peer = crm_element_value(node_state, XML_NODE_IS_PEER);
    const char *in_cluster = crm_element_value(node_state, XML_NODE_IN_CLUSTER);
    const char *exp_state = crm_element_value(node_state, XML_NODE_EXPECTED);

    if (!crm_is_true(in_cluster)) {
        crm_trace("Node is down: in_cluster=%s", crm_str(in_cluster));

    } else if (safe_str_eq(is_peer, ONLINESTATUS)) {
        if (safe_str_eq(join, CRMD_JOINSTATE_MEMBER)) {
            online = TRUE;
        } else {
            crm_debug("Node is not ready to run resources: %s", join);
        }

    } else if (this_node->details->expected_up == FALSE) {
        crm_trace("CRMd is down: in_cluster=%s", crm_str(in_cluster));
        crm_trace("\tis_peer=%s, join=%s, expected=%s",
                  crm_str(is_peer), crm_str(join), crm_str(exp_state));

    } else {
        /* mark it unclean */
        pe_fence_node(data_set, this_node, "peer is unexpectedly down");
        crm_info("\tin_cluster=%s, is_peer=%s, join=%s, expected=%s",
                 crm_str(in_cluster), crm_str(is_peer), crm_str(join), crm_str(exp_state));
    }
    return online;
}

static gboolean
determine_online_status_fencing(pe_working_set_t * data_set, xmlNode * node_state,
                                node_t * this_node)
{
    gboolean online = FALSE;
    gboolean do_terminate = FALSE;
    bool crmd_online = FALSE;
    const char *join = crm_element_value(node_state, XML_NODE_JOIN_STATE);
    const char *is_peer = crm_element_value(node_state, XML_NODE_IS_PEER);
    const char *in_cluster = crm_element_value(node_state, XML_NODE_IN_CLUSTER);
    const char *exp_state = crm_element_value(node_state, XML_NODE_EXPECTED);
    const char *terminate = pe_node_attribute_raw(this_node, "terminate");

/*
  - XML_NODE_IN_CLUSTER    ::= true|false
  - XML_NODE_IS_PEER       ::= online|offline
  - XML_NODE_JOIN_STATE    ::= member|down|pending|banned
  - XML_NODE_EXPECTED      ::= member|down
*/

    if (crm_is_true(terminate)) {
        do_terminate = TRUE;

    } else if (terminate != NULL && strlen(terminate) > 0) {
        /* could be a time() value */
        char t = terminate[0];

        if (t != '0' && isdigit(t)) {
            do_terminate = TRUE;
        }
    }

    crm_trace("%s: in_cluster=%s, is_peer=%s, join=%s, expected=%s, term=%d",
              this_node->details->uname, crm_str(in_cluster), crm_str(is_peer),
              crm_str(join), crm_str(exp_state), do_terminate);

    online = crm_is_true(in_cluster);
    crmd_online = safe_str_eq(is_peer, ONLINESTATUS);
    if (exp_state == NULL) {
        exp_state = CRMD_JOINSTATE_DOWN;
    }

    if (this_node->details->shutdown) {
        crm_debug("%s is shutting down", this_node->details->uname);

        /* Slightly different criteria since we can't shut down a dead peer */
        online = crmd_online;

    } else if (in_cluster == NULL) {
        pe_fence_node(data_set, this_node, "peer has not been seen by the cluster");

    } else if (safe_str_eq(join, CRMD_JOINSTATE_NACK)) {
        pe_fence_node(data_set, this_node, "peer failed the pacemaker membership criteria");

    } else if (do_terminate == FALSE && safe_str_eq(exp_state, CRMD_JOINSTATE_DOWN)) {

        if (crm_is_true(in_cluster) || crmd_online) {
            crm_info("- Node %s is not ready to run resources", this_node->details->uname);
            this_node->details->standby = TRUE;
            this_node->details->pending = TRUE;

        } else {
            crm_trace("%s is down or still coming up", this_node->details->uname);
        }

    } else if (do_terminate && safe_str_eq(join, CRMD_JOINSTATE_DOWN)
               && crm_is_true(in_cluster) == FALSE && !crmd_online) {
        crm_info("Node %s was just shot", this_node->details->uname);
        online = FALSE;

    } else if (crm_is_true(in_cluster) == FALSE) {
        pe_fence_node(data_set, this_node, "peer is no longer part of the cluster");

    } else if (!crmd_online) {
        pe_fence_node(data_set, this_node, "peer process is no longer available");

        /* Everything is running at this point, now check join state */
    } else if (do_terminate) {
        pe_fence_node(data_set, this_node, "termination was requested");

    } else if (safe_str_eq(join, CRMD_JOINSTATE_MEMBER)) {
        crm_info("Node %s is active", this_node->details->uname);

    } else if (safe_str_eq(join, CRMD_JOINSTATE_PENDING)
               || safe_str_eq(join, CRMD_JOINSTATE_DOWN)) {
        crm_info("Node %s is not ready to run resources", this_node->details->uname);
        this_node->details->standby = TRUE;
        this_node->details->pending = TRUE;

    } else {
        pe_fence_node(data_set, this_node, "peer was in an unknown state");
        crm_warn("%s: in-cluster=%s, is-peer=%s, join=%s, expected=%s, term=%d, shutdown=%d",
                 this_node->details->uname, crm_str(in_cluster), crm_str(is_peer),
                 crm_str(join), crm_str(exp_state), do_terminate, this_node->details->shutdown);
    }

    return online;
}

static gboolean
determine_remote_online_status(pe_working_set_t * data_set, node_t * this_node)
{
    resource_t *rsc = this_node->details->remote_rsc;
    resource_t *container = NULL;
    pe_node_t *host = NULL;

    /* If there is a node state entry for a (former) Pacemaker Remote node
     * but no resource creating that node, the node's connection resource will
     * be NULL. Consider it an offline remote node in that case.
     */
    if (rsc == NULL) {
        this_node->details->online = FALSE;
        goto remote_online_done;
    }

    container = rsc->container;

    if (container && (g_list_length(rsc->running_on) == 1)) {
        host = rsc->running_on->data;
    }

    /* If the resource is currently started, mark it online. */
    if (rsc->role == RSC_ROLE_STARTED) {
        crm_trace("%s node %s presumed ONLINE because connection resource is started",
                  (container? "Guest" : "Remote"), this_node->details->id);
        this_node->details->online = TRUE;
    }

    /* consider this node shutting down if transitioning start->stop */
    if (rsc->role == RSC_ROLE_STARTED && rsc->next_role == RSC_ROLE_STOPPED) {
        crm_trace("%s node %s shutting down because connection resource is stopping",
                  (container? "Guest" : "Remote"), this_node->details->id);
        this_node->details->shutdown = TRUE;
    }

    /* Now check all the failure conditions. */
    if(container && is_set(container->flags, pe_rsc_failed)) {
        crm_trace("Guest node %s UNCLEAN because guest resource failed",
                  this_node->details->id);
        this_node->details->online = FALSE;
        this_node->details->remote_requires_reset = TRUE;

    } else if(is_set(rsc->flags, pe_rsc_failed)) {
        crm_trace("%s node %s OFFLINE because connection resource failed",
                  (container? "Guest" : "Remote"), this_node->details->id);
        this_node->details->online = FALSE;

    } else if (rsc->role == RSC_ROLE_STOPPED
        || (container && container->role == RSC_ROLE_STOPPED)) {

        crm_trace("%s node %s OFFLINE because its resource is stopped",
                  (container? "Guest" : "Remote"), this_node->details->id);
        this_node->details->online = FALSE;
        this_node->details->remote_requires_reset = FALSE;

    } else if (host && (host->details->online == FALSE)
               && host->details->unclean) {
        crm_trace("Guest node %s UNCLEAN because host is unclean",
                  this_node->details->id);
        this_node->details->online = FALSE;
        this_node->details->remote_requires_reset = TRUE;
    }

remote_online_done:
    crm_trace("Remote node %s online=%s",
        this_node->details->id, this_node->details->online ? "TRUE" : "FALSE");
    return this_node->details->online;
}

gboolean
determine_online_status(xmlNode * node_state, node_t * this_node, pe_working_set_t * data_set)
{
    gboolean online = FALSE;
    const char *shutdown = NULL;
    const char *exp_state = crm_element_value(node_state, XML_NODE_EXPECTED);

    if (this_node == NULL) {
        crm_config_err("No node to check");
        return online;
    }

    this_node->details->shutdown = FALSE;
    this_node->details->expected_up = FALSE;
    shutdown = pe_node_attribute_raw(this_node, XML_CIB_ATTR_SHUTDOWN);

    if (shutdown != NULL && safe_str_neq("0", shutdown)) {
        this_node->details->shutdown = TRUE;

    } else if (safe_str_eq(exp_state, CRMD_JOINSTATE_MEMBER)) {
        this_node->details->expected_up = TRUE;
    }

    if (this_node->details->type == node_ping) {
        this_node->details->unclean = FALSE;
        online = FALSE;         /* As far as resource management is concerned,
                                 * the node is safely offline.
                                 * Anyone caught abusing this logic will be shot
                                 */

    } else if (is_set(data_set->flags, pe_flag_stonith_enabled) == FALSE) {
        online = determine_online_status_no_fencing(data_set, node_state, this_node);

    } else {
        online = determine_online_status_fencing(data_set, node_state, this_node);
    }

    if (online) {
        this_node->details->online = TRUE;

    } else {
        /* remove node from contention */
        this_node->fixed = TRUE;
        this_node->weight = -INFINITY;
    }

    if (online && this_node->details->shutdown) {
        /* don't run resources here */
        this_node->fixed = TRUE;
        this_node->weight = -INFINITY;
    }

    if (this_node->details->type == node_ping) {
        crm_info("Node %s is not a pacemaker node", this_node->details->uname);

    } else if (this_node->details->unclean) {
        pe_proc_warn("Node %s is unclean", this_node->details->uname);

    } else if (this_node->details->online) {
        crm_info("Node %s is %s", this_node->details->uname,
                 this_node->details->shutdown ? "shutting down" :
                 this_node->details->pending ? "pending" :
                 this_node->details->standby ? "standby" :
                 this_node->details->maintenance ? "maintenance" : "online");

    } else {
        crm_trace("Node %s is offline", this_node->details->uname);
    }

    return online;
}

/*!
 * \internal
 * \brief Find the end of a resource's name, excluding any clone suffix
 *
 * \param[in] id  Resource ID to check
 *
 * \return Pointer to last character of resource's base name
 */
const char *
pe_base_name_end(const char *id)
{
    if (!crm_strlen_zero(id)) {
        const char *end = id + strlen(id) - 1;

        for (const char *s = end; s > id; --s) {
            switch (*s) {
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                    break;
                case ':':
                    return (s == end)? s : (s - 1);
                default:
                    return end;
            }
        }
        return end;
    }
    return NULL;
}

/*!
 * \internal
 * \brief Get a resource name excluding any clone suffix
 *
 * \param[in] last_rsc_id  Resource ID to check
 *
 * \return Pointer to newly allocated string with resource's base name
 * \note It is the caller's responsibility to free() the result.
 *       This asserts on error, so callers can assume result is not NULL.
 */
char *
clone_strip(const char *last_rsc_id)
{
    const char *end = pe_base_name_end(last_rsc_id);
    char *basename = NULL;

    CRM_ASSERT(end);
    basename = strndup(last_rsc_id, end - last_rsc_id + 1);
    CRM_ASSERT(basename);
    return basename;
}

/*!
 * \internal
 * \brief Get the name of the first instance of a cloned resource
 *
 * \param[in] last_rsc_id  Resource ID to check
 *
 * \return Pointer to newly allocated string with resource's base name plus :0
 * \note It is the caller's responsibility to free() the result.
 *       This asserts on error, so callers can assume result is not NULL.
 */
char *
clone_zero(const char *last_rsc_id)
{
    const char *end = pe_base_name_end(last_rsc_id);
    size_t base_name_len = end - last_rsc_id + 1;
    char *zero = NULL;

    CRM_ASSERT(end);
    zero = calloc(base_name_len + 3, sizeof(char));
    CRM_ASSERT(zero);
    memcpy(zero, last_rsc_id, base_name_len);
    zero[base_name_len] = ':';
    zero[base_name_len + 1] = '0';
    return zero;
}

static resource_t *
create_fake_resource(const char *rsc_id, xmlNode * rsc_entry, pe_working_set_t * data_set)
{
    resource_t *rsc = NULL;
    xmlNode *xml_rsc = create_xml_node(NULL, XML_CIB_TAG_RESOURCE);

    copy_in_properties(xml_rsc, rsc_entry);
    crm_xml_add(xml_rsc, XML_ATTR_ID, rsc_id);
    crm_log_xml_debug(xml_rsc, "Orphan resource");

    if (!common_unpack(xml_rsc, &rsc, NULL, data_set)) {
        return NULL;
    }

    if (xml_contains_remote_node(xml_rsc)) {
        node_t *node;

        crm_debug("Detected orphaned remote node %s", rsc_id);
        node = pe_find_node(data_set->nodes, rsc_id);
        if (node == NULL) {
	        node = pe_create_node(rsc_id, rsc_id, "remote", NULL, data_set);
        }
        link_rsc2remotenode(data_set, rsc);

        if (node) {
            crm_trace("Setting node %s as shutting down due to orphaned connection resource", rsc_id);
            node->details->shutdown = TRUE;
        }
    }

    if (crm_element_value(rsc_entry, XML_RSC_ATTR_CONTAINER)) {
        /* This orphaned rsc needs to be mapped to a container. */
        crm_trace("Detected orphaned container filler %s", rsc_id);
        set_bit(rsc->flags, pe_rsc_orphan_container_filler);
    }
    set_bit(rsc->flags, pe_rsc_orphan);
    data_set->resources = g_list_append(data_set->resources, rsc);
    return rsc;
}

extern resource_t *create_child_clone(resource_t * rsc, int sub_id, pe_working_set_t * data_set);

static resource_t *
find_anonymous_clone(pe_working_set_t * data_set, node_t * node, resource_t * parent,
                     const char *rsc_id)
{
    GListPtr rIter = NULL;
    resource_t *rsc = NULL;
    gboolean skip_inactive = FALSE;

    CRM_ASSERT(parent != NULL);
    CRM_ASSERT(pe_rsc_is_clone(parent));
    CRM_ASSERT(is_not_set(parent->flags, pe_rsc_unique));

    /* Find an instance active (or partially active for grouped clones) on the specified node */
    pe_rsc_trace(parent, "Looking for %s on %s in %s", rsc_id, node->details->uname, parent->id);
    for (rIter = parent->children; rsc == NULL && rIter; rIter = rIter->next) {
        GListPtr nIter = NULL;
        GListPtr locations = NULL;
        resource_t *child = rIter->data;

        child->fns->location(child, &locations, TRUE);
        if (locations == NULL) {
            pe_rsc_trace(child, "Resource %s, skip inactive", child->id);
            continue;
        }

        for (nIter = locations; nIter && rsc == NULL; nIter = nIter->next) {
            node_t *childnode = nIter->data;

            if (childnode->details == node->details) {
                /* ->find_rsc() because we might be a cloned group */
                rsc = parent->fns->find_rsc(child, rsc_id, NULL, pe_find_clone);
                if(rsc) {
                    pe_rsc_trace(rsc, "Resource %s, active", rsc->id);
                }
            }

            /* Keep this block, it means we'll do the right thing if
             * anyone toggles the unique flag to 'off'
             */
            if (rsc && rsc->running_on) {
                crm_notice("/Anonymous/ clone %s is already running on %s",
                           parent->id, node->details->uname);
                skip_inactive = TRUE;
                rsc = NULL;
            }
        }

        g_list_free(locations);
    }

    /* Find an inactive instance */
    if (skip_inactive == FALSE) {
        pe_rsc_trace(parent, "Looking for %s anywhere", rsc_id);
        for (rIter = parent->children; rsc == NULL && rIter; rIter = rIter->next) {
            GListPtr locations = NULL;
            resource_t *child = rIter->data;

            if (is_set(child->flags, pe_rsc_block)) {
                pe_rsc_trace(child, "Skip: blocked in stopped state");
                continue;
            }

            child->fns->location(child, &locations, TRUE);
            if (locations == NULL) {
                /* ->find_rsc() because we might be a cloned group */
                rsc = parent->fns->find_rsc(child, rsc_id, NULL, pe_find_clone);
                pe_rsc_trace(parent, "Resource %s, empty slot", rsc->id);
            }
            g_list_free(locations);
        }
    }

    if (rsc == NULL) {
        /* Create an extra orphan */
        resource_t *top = create_child_clone(parent, -1, data_set);

        /* ->find_rsc() because we might be a cloned group */
        rsc = top->fns->find_rsc(top, rsc_id, NULL, pe_find_clone);
        CRM_ASSERT(rsc != NULL);

        pe_rsc_debug(parent, "Created orphan %s for %s: %s on %s", top->id, parent->id, rsc_id,
                     node->details->uname);
    }
    return rsc;
}

static resource_t *
unpack_find_resource(pe_working_set_t * data_set, node_t * node, const char *rsc_id,
                     xmlNode * rsc_entry)
{
    resource_t *rsc = NULL;
    resource_t *parent = NULL;

    crm_trace("looking for %s", rsc_id);
    rsc = pe_find_resource(data_set->resources, rsc_id);

    if (rsc == NULL) {
        /* If we didn't find the resource by its name in the operation history,
         * check it again as a clone instance. Even when clone-max=0, we create
         * a single :0 orphan to match against here.
         */
        char *clone0_id = clone_zero(rsc_id);
        resource_t *clone0 = pe_find_resource(data_set->resources, clone0_id);

        if (clone0 && is_not_set(clone0->flags, pe_rsc_unique)) {
            rsc = clone0;
        } else {
            crm_trace("%s is not known as %s either", rsc_id, clone0_id);
        }

        /* Grab the parent clone even if this a different unique instance,
         * so we can remember the clone name, which will be the same.
         */
        parent = uber_parent(clone0);
        free(clone0_id);

        crm_trace("%s not found: %s", rsc_id, parent ? parent->id : "orphan");

    } else if (rsc->variant > pe_native) {
        crm_trace("%s is no longer a primitive resource, the lrm_resource entry is obsolete",
                  rsc_id);
        return NULL;

    } else {
        parent = uber_parent(rsc);
    }

    if (pe_rsc_is_anon_clone(parent)) {

        if (parent && parent->parent) {
            rsc = find_container_child(parent->parent, node);
        } else {
            char *base = clone_strip(rsc_id);

            rsc = find_anonymous_clone(data_set, node, parent, base);
            free(base);
            CRM_ASSERT(rsc != NULL);
        }
    }

    if (rsc && safe_str_neq(rsc_id, rsc->id)
        && safe_str_neq(rsc_id, rsc->clone_name)) {

        free(rsc->clone_name);
        rsc->clone_name = strdup(rsc_id);
        pe_rsc_debug(rsc, "Internally renamed %s on %s to %s%s",
                     rsc_id, node->details->uname, rsc->id,
                     (is_set(rsc->flags, pe_rsc_orphan)? " (ORPHAN)" : ""));
    }
    return rsc;
}

static resource_t *
process_orphan_resource(xmlNode * rsc_entry, node_t * node, pe_working_set_t * data_set)
{
    resource_t *rsc = NULL;
    const char *rsc_id = crm_element_value(rsc_entry, XML_ATTR_ID);

    crm_debug("Detected orphan resource %s on %s", rsc_id, node->details->uname);
    rsc = create_fake_resource(rsc_id, rsc_entry, data_set);

    if (is_set(data_set->flags, pe_flag_stop_rsc_orphans) == FALSE) {
        clear_bit(rsc->flags, pe_rsc_managed);

    } else {
        print_resource(LOG_TRACE, "Added orphan", rsc, FALSE);

        CRM_CHECK(rsc != NULL, return NULL);
        resource_location(rsc, NULL, -INFINITY, "__orphan_dont_run__", data_set);
    }
    return rsc;
}

static void
process_rsc_state(resource_t * rsc, node_t * node,
                  enum action_fail_response on_fail,
                  xmlNode * migrate_op, pe_working_set_t * data_set)
{
    node_t *tmpnode = NULL;
    char *reason = NULL;

    CRM_ASSERT(rsc);
    pe_rsc_trace(rsc, "Resource %s is %s on %s: on_fail=%s",
                 rsc->id, role2text(rsc->role), node->details->uname, fail2text(on_fail));

    /* process current state */
    if (rsc->role != RSC_ROLE_UNKNOWN) {
        resource_t *iter = rsc;

        while (iter) {
            if (g_hash_table_lookup(iter->known_on, node->details->id) == NULL) {
                node_t *n = node_copy(node);

                pe_rsc_trace(rsc, "%s (aka. %s) known on %s", rsc->id, rsc->clone_name,
                             n->details->uname);
                g_hash_table_insert(iter->known_on, (gpointer) n->details->id, n);
            }
            if (is_set(iter->flags, pe_rsc_unique)) {
                break;
            }
            iter = iter->parent;
        }
    }

    /* If a managed resource is believed to be running, but node is down ... */
    if (rsc->role > RSC_ROLE_STOPPED
        && node->details->online == FALSE
        && node->details->maintenance == FALSE
        && is_set(rsc->flags, pe_rsc_managed)) {

        gboolean should_fence = FALSE;

        /* If this is a guest node, fence it (regardless of whether fencing is
         * enabled, because guest node fencing is done by recovery of the
         * container resource rather than by stonithd). Mark the resource
         * we're processing as failed. When the guest comes back up, its
         * operation history in the CIB will be cleared, freeing the affected
         * resource to run again once we are sure we know its state.
         */
        if (is_container_remote_node(node)) {
            set_bit(rsc->flags, pe_rsc_failed);
            should_fence = TRUE;

        } else if (is_set(data_set->flags, pe_flag_stonith_enabled)) {
            if (is_baremetal_remote_node(node) && node->details->remote_rsc
                && is_not_set(node->details->remote_rsc->flags, pe_rsc_failed)) {

                /* setting unseen = true means that fencing of the remote node will
                 * only occur if the connection resource is not going to start somewhere.
                 * This allows connection resources on a failed cluster-node to move to
                 * another node without requiring the baremetal remote nodes to be fenced
                 * as well. */
                node->details->unseen = TRUE;
                reason = crm_strdup_printf("%s is active there (fencing will be"
                                           " revoked if remote connection can "
                                           "be re-established elsewhere)",
                                           rsc->id);
            }
            should_fence = TRUE;
        }

        if (should_fence) {
            if (reason == NULL) {
               reason = crm_strdup_printf("%s is thought to be active there", rsc->id);
            }
            pe_fence_node(data_set, node, reason);
        }
        free(reason);
    }

    if (node->details->unclean) {
        /* No extra processing needed
         * Also allows resources to be started again after a node is shot
         */
        on_fail = action_fail_ignore;
    }

    switch (on_fail) {
        case action_fail_ignore:
            /* nothing to do */
            break;

        case action_fail_fence:
            /* treat it as if it is still running
             * but also mark the node as unclean
             */
            reason = crm_strdup_printf("%s failed there", rsc->id);
            pe_fence_node(data_set, node, reason);
            free(reason);
            break;

        case action_fail_standby:
            node->details->standby = TRUE;
            node->details->standby_onfail = TRUE;
            break;

        case action_fail_block:
            /* is_managed == FALSE will prevent any
             * actions being sent for the resource
             */
            clear_bit(rsc->flags, pe_rsc_managed);
            set_bit(rsc->flags, pe_rsc_block);
            break;

        case action_fail_migrate:
            /* make sure it comes up somewhere else
             * or not at all
             */
            resource_location(rsc, node, -INFINITY, "__action_migration_auto__", data_set);
            break;

        case action_fail_stop:
            rsc->next_role = RSC_ROLE_STOPPED;
            break;

        case action_fail_recover:
            if (rsc->role != RSC_ROLE_STOPPED && rsc->role != RSC_ROLE_UNKNOWN) {
                set_bit(rsc->flags, pe_rsc_failed);
                stop_action(rsc, node, FALSE);
            }
            break;

        case action_fail_restart_container:
            set_bit(rsc->flags, pe_rsc_failed);

            if (rsc->container) {
                stop_action(rsc->container, node, FALSE);
            } else if (rsc->role != RSC_ROLE_STOPPED && rsc->role != RSC_ROLE_UNKNOWN) {
                stop_action(rsc, node, FALSE);
            }
            break;

        case action_fail_reset_remote:
            set_bit(rsc->flags, pe_rsc_failed);
            if (is_set(data_set->flags, pe_flag_stonith_enabled)) {
                tmpnode = NULL;
                if (rsc->is_remote_node) {
                    tmpnode = pe_find_node(data_set->nodes, rsc->id);
                }
                if (tmpnode &&
                    is_baremetal_remote_node(tmpnode) &&
                    tmpnode->details->remote_was_fenced == 0) {

                    /* connection resource to baremetal resource failed in a way that
                     * should result in fencing the remote-node. */
                    pe_fence_node(data_set, tmpnode,
                                  "remote connection is unrecoverable");
                }
            }

            /* require the stop action regardless if fencing is occurring or not. */
            if (rsc->role > RSC_ROLE_STOPPED) {
                stop_action(rsc, node, FALSE);
            }

            /* if reconnect delay is in use, prevent the connection from exiting the
             * "STOPPED" role until the failure is cleared by the delay timeout. */
            if (rsc->remote_reconnect_ms) {
                rsc->next_role = RSC_ROLE_STOPPED;
            }
            break;
    }

    /* ensure a remote-node connection failure forces an unclean remote-node
     * to be fenced. By setting unseen = FALSE, the remote-node failure will
     * result in a fencing operation regardless if we're going to attempt to 
     * reconnect to the remote-node in this transition or not. */
    if (is_set(rsc->flags, pe_rsc_failed) && rsc->is_remote_node) {
        tmpnode = pe_find_node(data_set->nodes, rsc->id);
        if (tmpnode && tmpnode->details->unclean) {
            tmpnode->details->unseen = FALSE;
        }
    }

    if (rsc->role != RSC_ROLE_STOPPED && rsc->role != RSC_ROLE_UNKNOWN) {
        if (is_set(rsc->flags, pe_rsc_orphan)) {
            if (is_set(rsc->flags, pe_rsc_managed)) {
                crm_config_warn("Detected active orphan %s running on %s",
                                rsc->id, node->details->uname);
            } else {
                crm_config_warn("Cluster configured not to stop active orphans."
                                " %s must be stopped manually on %s",
                                rsc->id, node->details->uname);
            }
        }

        native_add_running(rsc, node, data_set);
        if (on_fail != action_fail_ignore) {
            set_bit(rsc->flags, pe_rsc_failed);
        }

    } else if (rsc->clone_name && strchr(rsc->clone_name, ':') != NULL) {
        /* Only do this for older status sections that included instance numbers
         * Otherwise stopped instances will appear as orphans
         */
        pe_rsc_trace(rsc, "Resetting clone_name %s for %s (stopped)", rsc->clone_name, rsc->id);
        free(rsc->clone_name);
        rsc->clone_name = NULL;

    } else {
        char *key = stop_key(rsc);
        GListPtr possible_matches = find_actions(rsc->actions, key, node);
        GListPtr gIter = possible_matches;

        for (; gIter != NULL; gIter = gIter->next) {
            action_t *stop = (action_t *) gIter->data;

            stop->flags |= pe_action_optional;
        }

        g_list_free(possible_matches);
        free(key);
    }
}

/* create active recurring operations as optional */
static void
process_recurring(node_t * node, resource_t * rsc,
                  int start_index, int stop_index,
                  GListPtr sorted_op_list, pe_working_set_t * data_set)
{
    int counter = -1;
    const char *task = NULL;
    const char *status = NULL;
    GListPtr gIter = sorted_op_list;

    CRM_ASSERT(rsc);
    pe_rsc_trace(rsc, "%s: Start index %d, stop index = %d", rsc->id, start_index, stop_index);

    for (; gIter != NULL; gIter = gIter->next) {
        xmlNode *rsc_op = (xmlNode *) gIter->data;

        guint interval_ms = 0;
        char *key = NULL;
        const char *id = ID(rsc_op);
        const char *interval_ms_s = NULL;

        counter++;

        if (node->details->online == FALSE) {
            pe_rsc_trace(rsc, "Skipping %s/%s: node is offline", rsc->id, node->details->uname);
            break;

            /* Need to check if there's a monitor for role="Stopped" */
        } else if (start_index < stop_index && counter <= stop_index) {
            pe_rsc_trace(rsc, "Skipping %s/%s: resource is not active", id, node->details->uname);
            continue;

        } else if (counter < start_index) {
            pe_rsc_trace(rsc, "Skipping %s/%s: old %d", id, node->details->uname, counter);
            continue;
        }

        interval_ms_s = crm_element_value(rsc_op, XML_LRM_ATTR_INTERVAL_MS);
        interval_ms = crm_parse_ms(interval_ms_s);
        if (interval_ms == 0) {
            pe_rsc_trace(rsc, "Skipping %s/%s: non-recurring", id, node->details->uname);
            continue;
        }

        status = crm_element_value(rsc_op, XML_LRM_ATTR_OPSTATUS);
        if (safe_str_eq(status, "-1")) {
            pe_rsc_trace(rsc, "Skipping %s/%s: status", id, node->details->uname);
            continue;
        }
        task = crm_element_value(rsc_op, XML_LRM_ATTR_TASK);
        /* create the action */
        key = generate_op_key(rsc->id, task, interval_ms);
        pe_rsc_trace(rsc, "Creating %s/%s", key, node->details->uname);
        custom_action(rsc, key, task, node, TRUE, TRUE, data_set);
    }
}

void
calculate_active_ops(GListPtr sorted_op_list, int *start_index, int *stop_index)
{
    int counter = -1;
    int implied_monitor_start = -1;
    int implied_clone_start = -1;
    const char *task = NULL;
    const char *status = NULL;
    GListPtr gIter = sorted_op_list;

    *stop_index = -1;
    *start_index = -1;

    for (; gIter != NULL; gIter = gIter->next) {
        xmlNode *rsc_op = (xmlNode *) gIter->data;

        counter++;

        task = crm_element_value(rsc_op, XML_LRM_ATTR_TASK);
        status = crm_element_value(rsc_op, XML_LRM_ATTR_OPSTATUS);

        if (safe_str_eq(task, CRMD_ACTION_STOP)
            && safe_str_eq(status, "0")) {
            *stop_index = counter;

        } else if (safe_str_eq(task, CRMD_ACTION_START) || safe_str_eq(task, CRMD_ACTION_MIGRATED)) {
            *start_index = counter;

        } else if ((implied_monitor_start <= *stop_index) && safe_str_eq(task, CRMD_ACTION_STATUS)) {
            const char *rc = crm_element_value(rsc_op, XML_LRM_ATTR_RC);

            if (safe_str_eq(rc, "0") || safe_str_eq(rc, "8")) {
                implied_monitor_start = counter;
            }
        } else if (safe_str_eq(task, CRMD_ACTION_PROMOTE) || safe_str_eq(task, CRMD_ACTION_DEMOTE)) {
            implied_clone_start = counter;
        }
    }

    if (*start_index == -1) {
        if (implied_clone_start != -1) {
            *start_index = implied_clone_start;
        } else if (implied_monitor_start != -1) {
            *start_index = implied_monitor_start;
        }
    }
}

static resource_t *
unpack_lrm_rsc_state(node_t * node, xmlNode * rsc_entry, pe_working_set_t * data_set)
{
    GListPtr gIter = NULL;
    int stop_index = -1;
    int start_index = -1;
    enum rsc_role_e req_role = RSC_ROLE_UNKNOWN;

    const char *task = NULL;
    const char *rsc_id = crm_element_value(rsc_entry, XML_ATTR_ID);

    resource_t *rsc = NULL;
    GListPtr op_list = NULL;
    GListPtr sorted_op_list = NULL;

    xmlNode *migrate_op = NULL;
    xmlNode *rsc_op = NULL;
    xmlNode *last_failure = NULL;

    enum action_fail_response on_fail = FALSE;
    enum rsc_role_e saved_role = RSC_ROLE_UNKNOWN;

    crm_trace("[%s] Processing %s on %s",
              crm_element_name(rsc_entry), rsc_id, node->details->uname);

    /* extract operations */
    op_list = NULL;
    sorted_op_list = NULL;

    for (rsc_op = __xml_first_child(rsc_entry); rsc_op != NULL; rsc_op = __xml_next_element(rsc_op)) {
        if (crm_str_eq((const char *)rsc_op->name, XML_LRM_TAG_RSC_OP, TRUE)) {
            op_list = g_list_prepend(op_list, rsc_op);
        }
    }

    if (op_list == NULL) {
        /* if there are no operations, there is nothing to do */
        return NULL;
    }

    /* find the resource */
    rsc = unpack_find_resource(data_set, node, rsc_id, rsc_entry);
    if (rsc == NULL) {
        rsc = process_orphan_resource(rsc_entry, node, data_set);
    }
    CRM_ASSERT(rsc != NULL);

    /* process operations */
    saved_role = rsc->role;
    on_fail = action_fail_ignore;
    rsc->role = RSC_ROLE_UNKNOWN;
    sorted_op_list = g_list_sort(op_list, sort_op_by_callid);

    for (gIter = sorted_op_list; gIter != NULL; gIter = gIter->next) {
        xmlNode *rsc_op = (xmlNode *) gIter->data;

        task = crm_element_value(rsc_op, XML_LRM_ATTR_TASK);
        if (safe_str_eq(task, CRMD_ACTION_MIGRATED)) {
            migrate_op = rsc_op;
        }

        unpack_rsc_op(rsc, node, rsc_op, &last_failure, &on_fail, data_set);
    }

    /* create active recurring operations as optional */
    calculate_active_ops(sorted_op_list, &start_index, &stop_index);
    process_recurring(node, rsc, start_index, stop_index, sorted_op_list, data_set);

    /* no need to free the contents */
    g_list_free(sorted_op_list);

    process_rsc_state(rsc, node, on_fail, migrate_op, data_set);

    if (get_target_role(rsc, &req_role)) {
        if (rsc->next_role == RSC_ROLE_UNKNOWN || req_role < rsc->next_role) {
            pe_rsc_debug(rsc, "%s: Overwriting calculated next role %s"
                         " with requested next role %s",
                         rsc->id, role2text(rsc->next_role), role2text(req_role));
            rsc->next_role = req_role;

        } else if (req_role > rsc->next_role) {
            pe_rsc_info(rsc, "%s: Not overwriting calculated next role %s"
                        " with requested next role %s",
                        rsc->id, role2text(rsc->next_role), role2text(req_role));
        }
    }

    if (saved_role > rsc->role) {
        rsc->role = saved_role;
    }

    return rsc;
}

static void
handle_orphaned_container_fillers(xmlNode * lrm_rsc_list, pe_working_set_t * data_set)
{
    xmlNode *rsc_entry = NULL;
    for (rsc_entry = __xml_first_child(lrm_rsc_list); rsc_entry != NULL;
        rsc_entry = __xml_next_element(rsc_entry)) {

        resource_t *rsc;
        resource_t *container;
        const char *rsc_id;
        const char *container_id;

        if (safe_str_neq((const char *)rsc_entry->name, XML_LRM_TAG_RESOURCE)) {
            continue;
        }

        container_id = crm_element_value(rsc_entry, XML_RSC_ATTR_CONTAINER);
        rsc_id = crm_element_value(rsc_entry, XML_ATTR_ID);
        if (container_id == NULL || rsc_id == NULL) {
            continue;
        }

        container = pe_find_resource(data_set->resources, container_id);
        if (container == NULL) {
            continue;
        }

        rsc = pe_find_resource(data_set->resources, rsc_id);
        if (rsc == NULL ||
            is_set(rsc->flags, pe_rsc_orphan_container_filler) == FALSE ||
            rsc->container != NULL) {
            continue;
        }

        pe_rsc_trace(rsc, "Mapped orphaned rsc %s's container to  %s", rsc->id, container_id);
        rsc->container = container;
        container->fillers = g_list_append(container->fillers, rsc);
    }
}

gboolean
unpack_lrm_resources(node_t * node, xmlNode * lrm_rsc_list, pe_working_set_t * data_set)
{
    xmlNode *rsc_entry = NULL;
    gboolean found_orphaned_container_filler = FALSE;

    CRM_CHECK(node != NULL, return FALSE);

    crm_trace("Unpacking resources on %s", node->details->uname);

    for (rsc_entry = __xml_first_child(lrm_rsc_list); rsc_entry != NULL;
         rsc_entry = __xml_next_element(rsc_entry)) {

        if (crm_str_eq((const char *)rsc_entry->name, XML_LRM_TAG_RESOURCE, TRUE)) {
            resource_t *rsc = unpack_lrm_rsc_state(node, rsc_entry, data_set);
            if (!rsc) {
                continue;
            }
            if (is_set(rsc->flags, pe_rsc_orphan_container_filler)) {
                found_orphaned_container_filler = TRUE;
            }
        }
    }

    /* now that all the resource state has been unpacked for this node
     * we have to go back and map any orphaned container fillers to their
     * container resource */
    if (found_orphaned_container_filler) {
        handle_orphaned_container_fillers(lrm_rsc_list, data_set);
    }
    return TRUE;
}

static void
set_active(resource_t * rsc)
{
    resource_t *top = uber_parent(rsc);

    if (top && is_set(top->flags, pe_rsc_promotable)) {
        rsc->role = RSC_ROLE_SLAVE;
    } else {
        rsc->role = RSC_ROLE_STARTED;
    }
}

static void
set_node_score(gpointer key, gpointer value, gpointer user_data)
{
    node_t *node = value;
    int *score = user_data;

    node->weight = *score;
}

#define STATUS_PATH_MAX 1024
static xmlNode *
find_lrm_op(const char *resource, const char *op, const char *node, const char *source,
            pe_working_set_t * data_set)
{
    int offset = 0;
    char xpath[STATUS_PATH_MAX];

    offset += snprintf(xpath + offset, STATUS_PATH_MAX - offset, "//node_state[@uname='%s']", node);
    offset +=
        snprintf(xpath + offset, STATUS_PATH_MAX - offset, "//" XML_LRM_TAG_RESOURCE "[@id='%s']",
                 resource);

    /* Need to check against transition_magic too? */
    if (source && safe_str_eq(op, CRMD_ACTION_MIGRATE)) {
        offset +=
            snprintf(xpath + offset, STATUS_PATH_MAX - offset,
                     "/" XML_LRM_TAG_RSC_OP "[@operation='%s' and @migrate_target='%s']", op,
                     source);
    } else if (source && safe_str_eq(op, CRMD_ACTION_MIGRATED)) {
        offset +=
            snprintf(xpath + offset, STATUS_PATH_MAX - offset,
                     "/" XML_LRM_TAG_RSC_OP "[@operation='%s' and @migrate_source='%s']", op,
                     source);
    } else {
        offset +=
            snprintf(xpath + offset, STATUS_PATH_MAX - offset,
                     "/" XML_LRM_TAG_RSC_OP "[@operation='%s']", op);
    }

    CRM_LOG_ASSERT(offset > 0);
    return get_xpath_object(xpath, data_set->input, LOG_DEBUG);
}

static void
unpack_rsc_migration(resource_t *rsc, node_t *node, xmlNode *xml_op, pe_working_set_t * data_set)
{

    /*
     * The normal sequence is (now): migrate_to(Src) -> migrate_from(Tgt) -> stop(Src)
     *
     * So if a migrate_to is followed by a stop, then we don't need to care what
     * happened on the target node
     *
     * Without the stop, we need to look for a successful migrate_from.
     * This would also imply we're no longer running on the source
     *
     * Without the stop, and without a migrate_from op we make sure the resource
     * gets stopped on both source and target (assuming the target is up)
     *
     */
    int stop_id = 0;
    int task_id = 0;
    xmlNode *stop_op =
        find_lrm_op(rsc->id, CRMD_ACTION_STOP, node->details->id, NULL, data_set);

    if (stop_op) {
        crm_element_value_int(stop_op, XML_LRM_ATTR_CALLID, &stop_id);
    }

    crm_element_value_int(xml_op, XML_LRM_ATTR_CALLID, &task_id);

    if (stop_op == NULL || stop_id < task_id) {
        int from_rc = 0, from_status = 0;
        const char *migrate_source =
            crm_element_value(xml_op, XML_LRM_ATTR_MIGRATE_SOURCE);
        const char *migrate_target =
            crm_element_value(xml_op, XML_LRM_ATTR_MIGRATE_TARGET);

        node_t *target = pe_find_node(data_set->nodes, migrate_target);
        node_t *source = pe_find_node(data_set->nodes, migrate_source);
        xmlNode *migrate_from =
            find_lrm_op(rsc->id, CRMD_ACTION_MIGRATED, migrate_target, migrate_source,
                        data_set);

        rsc->role = RSC_ROLE_STARTED;       /* can be master? */
        if (migrate_from) {
            crm_element_value_int(migrate_from, XML_LRM_ATTR_RC, &from_rc);
            crm_element_value_int(migrate_from, XML_LRM_ATTR_OPSTATUS, &from_status);
            pe_rsc_trace(rsc, "%s op on %s exited with status=%d, rc=%d",
                         ID(migrate_from), migrate_target, from_status, from_rc);
        }

        if (migrate_from && from_rc == PCMK_OCF_OK
            && from_status == PCMK_LRM_OP_DONE) {
            pe_rsc_trace(rsc, "Detected dangling migration op: %s on %s", ID(xml_op),
                         migrate_source);

            /* all good
             * just need to arrange for the stop action to get sent
             * but _without_ affecting the target somehow
             */
            rsc->role = RSC_ROLE_STOPPED;
            rsc->dangling_migrations = g_list_prepend(rsc->dangling_migrations, node);

        } else if (migrate_from) {  /* Failed */
            if (target && target->details->online) {
                pe_rsc_trace(rsc, "Marking active on %s %p %d", migrate_target, target,
                             target->details->online);
                native_add_running(rsc, target, data_set);
            }

        } else {    /* Pending or complete but erased */
            if (target && target->details->online) {
                pe_rsc_trace(rsc, "Marking active on %s %p %d", migrate_target, target,
                             target->details->online);

                native_add_running(rsc, target, data_set);
                if (source && source->details->online) {
                    /* If we make it here we have a partial migration.  The migrate_to
                     * has completed but the migrate_from on the target has not. Hold on
                     * to the target and source on the resource. Later on if we detect that
                     * the resource is still going to run on that target, we may continue
                     * the migration */
                    rsc->partial_migration_target = target;
                    rsc->partial_migration_source = source;
                }
            } else {
                /* Consider it failed here - forces a restart, prevents migration */
                set_bit(rsc->flags, pe_rsc_failed);
                clear_bit(rsc->flags, pe_rsc_allow_migrate);
            }
        }
    }
}

static void
unpack_rsc_migration_failure(resource_t *rsc, node_t *node, xmlNode *xml_op, pe_working_set_t * data_set) 
{
    const char *task = crm_element_value(xml_op, XML_LRM_ATTR_TASK);

    CRM_ASSERT(rsc);
    if (safe_str_eq(task, CRMD_ACTION_MIGRATED)) {
        int stop_id = 0;
        int migrate_id = 0;
        const char *migrate_source = crm_element_value(xml_op, XML_LRM_ATTR_MIGRATE_SOURCE);
        const char *migrate_target = crm_element_value(xml_op, XML_LRM_ATTR_MIGRATE_TARGET);

        xmlNode *stop_op =
            find_lrm_op(rsc->id, CRMD_ACTION_STOP, migrate_source, NULL, data_set);
        xmlNode *migrate_op =
            find_lrm_op(rsc->id, CRMD_ACTION_MIGRATE, migrate_source, migrate_target,
                        data_set);

        if (stop_op) {
            crm_element_value_int(stop_op, XML_LRM_ATTR_CALLID, &stop_id);
        }
        if (migrate_op) {
            crm_element_value_int(migrate_op, XML_LRM_ATTR_CALLID, &migrate_id);
        }

        /* Get our state right */
        rsc->role = RSC_ROLE_STARTED;   /* can be master? */

        if (stop_op == NULL || stop_id < migrate_id) {
            node_t *source = pe_find_node(data_set->nodes, migrate_source);

            if (source && source->details->online) {
                native_add_running(rsc, source, data_set);
            }
        }

    } else if (safe_str_eq(task, CRMD_ACTION_MIGRATE)) {
        int stop_id = 0;
        int migrate_id = 0;
        const char *migrate_source = crm_element_value(xml_op, XML_LRM_ATTR_MIGRATE_SOURCE);
        const char *migrate_target = crm_element_value(xml_op, XML_LRM_ATTR_MIGRATE_TARGET);

        xmlNode *stop_op =
            find_lrm_op(rsc->id, CRMD_ACTION_STOP, migrate_target, NULL, data_set);
        xmlNode *migrate_op =
            find_lrm_op(rsc->id, CRMD_ACTION_MIGRATED, migrate_target, migrate_source,
                        data_set);

        if (stop_op) {
            crm_element_value_int(stop_op, XML_LRM_ATTR_CALLID, &stop_id);
        }
        if (migrate_op) {
            crm_element_value_int(migrate_op, XML_LRM_ATTR_CALLID, &migrate_id);
        }

        /* Get our state right */
        rsc->role = RSC_ROLE_STARTED;   /* can be master? */

        if (stop_op == NULL || stop_id < migrate_id) {
            node_t *target = pe_find_node(data_set->nodes, migrate_target);

            pe_rsc_trace(rsc, "Stop: %p %d, Migrated: %p %d", stop_op, stop_id, migrate_op,
                         migrate_id);
            if (target && target->details->online) {
                native_add_running(rsc, target, data_set);
            }

        } else if (migrate_op == NULL) {
            /* Make sure it gets cleaned up, the stop may pre-date the migrate_from */
            rsc->dangling_migrations = g_list_prepend(rsc->dangling_migrations, node);
        }
    }
}

static void
record_failed_op(xmlNode *op, node_t* node, resource_t *rsc, pe_working_set_t * data_set)
{
    xmlNode *xIter = NULL;
    const char *op_key = crm_element_value(op, XML_LRM_ATTR_TASK_KEY);

    if (node->details->online == FALSE) {
        return;
    }

    for (xIter = data_set->failed->children; xIter; xIter = xIter->next) {
        const char *key = crm_element_value(xIter, XML_LRM_ATTR_TASK_KEY);
        const char *uname = crm_element_value(xIter, XML_ATTR_UNAME);

        if(safe_str_eq(op_key, key) && safe_str_eq(uname, node->details->uname)) {
            crm_trace("Skipping duplicate entry %s on %s", op_key, node->details->uname);
            return;
        }
    }

    crm_trace("Adding entry %s on %s", op_key, node->details->uname);
    crm_xml_add(op, XML_ATTR_UNAME, node->details->uname);
    crm_xml_add(op, XML_LRM_ATTR_RSCID, rsc->id);
    add_node_copy(data_set->failed, op);
}

static const char *get_op_key(xmlNode *xml_op)
{
    const char *key = crm_element_value(xml_op, XML_LRM_ATTR_TASK_KEY);
    if(key == NULL) {
        key = ID(xml_op);
    }
    return key;
}

static void
unpack_rsc_op_failure(resource_t * rsc, node_t * node, int rc, xmlNode * xml_op, xmlNode ** last_failure,
                      enum action_fail_response * on_fail, pe_working_set_t * data_set)
{
    guint interval_ms = 0;
    bool is_probe = FALSE;
    action_t *action = NULL;

    const char *key = get_op_key(xml_op);
    const char *task = crm_element_value(xml_op, XML_LRM_ATTR_TASK);

    CRM_ASSERT(rsc);

    *last_failure = xml_op;

    crm_element_value_ms(xml_op, XML_LRM_ATTR_INTERVAL_MS, &interval_ms);
    if ((interval_ms == 0) && safe_str_eq(task, CRMD_ACTION_STATUS)) {
        is_probe = TRUE;
        pe_rsc_trace(rsc, "is a probe: %s", key);
    }

    if (rc != PCMK_OCF_NOT_INSTALLED || is_set(data_set->flags, pe_flag_symmetric_cluster)) {
        crm_warn("Processing failed op %s for %s on %s: %s (%d)",
                 task, rsc->id, node->details->uname, services_ocf_exitcode_str(rc),
                 rc);

        record_failed_op(xml_op, node, rsc, data_set);

    } else {
        crm_trace("Processing failed op %s for %s on %s: %s (%d)",
                 task, rsc->id, node->details->uname, services_ocf_exitcode_str(rc),
                 rc);
    }

    action = custom_action(rsc, strdup(key), task, NULL, TRUE, FALSE, data_set);
    if ((action->on_fail <= action_fail_fence && *on_fail < action->on_fail) ||
        (action->on_fail == action_fail_reset_remote && *on_fail <= action_fail_recover) ||
        (action->on_fail == action_fail_restart_container && *on_fail <= action_fail_recover) ||
        (*on_fail == action_fail_restart_container && action->on_fail >= action_fail_migrate)) {
        pe_rsc_trace(rsc, "on-fail %s -> %s for %s (%s)", fail2text(*on_fail),
                     fail2text(action->on_fail), action->uuid, key);
        *on_fail = action->on_fail;
    }

    if (safe_str_eq(task, CRMD_ACTION_STOP)) {
        resource_location(rsc, node, -INFINITY, "__stop_fail__", data_set);

    } else if (safe_str_eq(task, CRMD_ACTION_MIGRATE) || safe_str_eq(task, CRMD_ACTION_MIGRATED)) {
        unpack_rsc_migration_failure(rsc, node, xml_op, data_set);

    } else if (safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
        rsc->role = RSC_ROLE_MASTER;

    } else if (safe_str_eq(task, CRMD_ACTION_DEMOTE)) {
        if (action->on_fail == action_fail_block) {
            rsc->role = RSC_ROLE_MASTER;
            rsc->next_role = RSC_ROLE_STOPPED;

        } else if(rc == PCMK_OCF_NOT_RUNNING) {
            rsc->role = RSC_ROLE_STOPPED;

        } else {
            /*
             * Staying in master role would put the PE/TE into a loop. Setting
             * slave role is not dangerous because the resource will be stopped
             * as part of recovery, and any master promotion will be ordered
             * after that stop.
             */
            rsc->role = RSC_ROLE_SLAVE;
        }
    }

    if(is_probe && rc == PCMK_OCF_NOT_INSTALLED) {
        /* leave stopped */
        pe_rsc_trace(rsc, "Leaving %s stopped", rsc->id);
        rsc->role = RSC_ROLE_STOPPED;

    } else if (rsc->role < RSC_ROLE_STARTED) {
        pe_rsc_trace(rsc, "Setting %s active", rsc->id);
        set_active(rsc);
    }

    pe_rsc_trace(rsc, "Resource %s: role=%s, unclean=%s, on_fail=%s, fail_role=%s",
                 rsc->id, role2text(rsc->role),
                 node->details->unclean ? "true" : "false",
                 fail2text(action->on_fail), role2text(action->fail_role));

    if (action->fail_role != RSC_ROLE_STARTED && rsc->next_role < action->fail_role) {
        rsc->next_role = action->fail_role;
    }

    if (action->fail_role == RSC_ROLE_STOPPED) {
        int score = -INFINITY;

        resource_t *fail_rsc = rsc;

        if (fail_rsc->parent) {
            resource_t *parent = uber_parent(fail_rsc);

            if (pe_rsc_is_clone(parent)
                && is_not_set(parent->flags, pe_rsc_unique)) {
                /* For clone resources, if a child fails on an operation
                 * with on-fail = stop, all the resources fail.  Do this by preventing
                 * the parent from coming up again. */
                fail_rsc = parent;
            }
        }
        crm_warn("Making sure %s doesn't come up again", fail_rsc->id);
        /* make sure it doesn't come up again */
        g_hash_table_destroy(fail_rsc->allowed_nodes);
        fail_rsc->allowed_nodes = node_hash_from_list(data_set->nodes);
        g_hash_table_foreach(fail_rsc->allowed_nodes, set_node_score, &score);
    }

    pe_free_action(action);
}

static int
determine_op_status(
    resource_t *rsc, int rc, int target_rc, node_t * node, xmlNode * xml_op, enum action_fail_response * on_fail, pe_working_set_t * data_set) 
{
    guint interval_ms = 0;
    int result = PCMK_LRM_OP_DONE;

    const char *key = get_op_key(xml_op);
    const char *task = crm_element_value(xml_op, XML_LRM_ATTR_TASK);

    bool is_probe = FALSE;

    CRM_ASSERT(rsc);
    crm_element_value_ms(xml_op, XML_LRM_ATTR_INTERVAL_MS, &interval_ms);
    if ((interval_ms == 0) && safe_str_eq(task, CRMD_ACTION_STATUS)) {
        is_probe = TRUE;
    }

    if (target_rc >= 0 && target_rc != rc) {
        result = PCMK_LRM_OP_ERROR;
        pe_rsc_debug(rsc, "%s on %s returned '%s' (%d) instead of the expected value: '%s' (%d)",
                     key, node->details->uname,
                     services_ocf_exitcode_str(rc), rc,
                     services_ocf_exitcode_str(target_rc), target_rc);
    }

    /* we could clean this up significantly except for old LRMs and CRMs that
     * didn't include target_rc and liked to remap status
     */
    switch (rc) {
        case PCMK_OCF_OK:
            if (is_probe && target_rc == 7) {
                result = PCMK_LRM_OP_DONE;
                pe_rsc_info(rsc, "Operation %s found resource %s active on %s",
                            task, rsc->id, node->details->uname);
            }
            break;

        case PCMK_OCF_NOT_RUNNING:
            if (is_probe || target_rc == rc || is_not_set(rsc->flags, pe_rsc_managed)) {
                result = PCMK_LRM_OP_DONE;
                rsc->role = RSC_ROLE_STOPPED;

                /* clear any previous failure actions */
                *on_fail = action_fail_ignore;
                rsc->next_role = RSC_ROLE_UNKNOWN;

            } else if (safe_str_neq(task, CRMD_ACTION_STOP)) {
                result = PCMK_LRM_OP_ERROR;
            }
            break;

        case PCMK_OCF_RUNNING_MASTER:
            if (is_probe) {
                result = PCMK_LRM_OP_DONE;
                pe_rsc_info(rsc, "Operation %s found resource %s active in master mode on %s",
                            task, rsc->id, node->details->uname);

            } else if (target_rc == rc) {
                /* nothing to do */

            } else if (target_rc >= 0) {
                result = PCMK_LRM_OP_ERROR;
            }
            rsc->role = RSC_ROLE_MASTER;
            break;

        case PCMK_OCF_DEGRADED_MASTER:
        case PCMK_OCF_FAILED_MASTER:
            rsc->role = RSC_ROLE_MASTER;
            result = PCMK_LRM_OP_ERROR;
            break;

        case PCMK_OCF_NOT_CONFIGURED:
            result = PCMK_LRM_OP_ERROR_FATAL;
            break;

        case PCMK_OCF_NOT_INSTALLED:
        case PCMK_OCF_INVALID_PARAM:
        case PCMK_OCF_INSUFFICIENT_PRIV:
        case PCMK_OCF_UNIMPLEMENT_FEATURE:
            if (rc == PCMK_OCF_UNIMPLEMENT_FEATURE && (interval_ms > 0)) {
                result = PCMK_LRM_OP_NOTSUPPORTED;
                break;

            } else if (pe_can_fence(data_set, node) == FALSE
               && safe_str_eq(task, CRMD_ACTION_STOP)) {
                /* If a stop fails and we can't fence, there's nothing else we can do */
                pe_proc_err("No further recovery can be attempted for %s: %s action failed with '%s' (%d)",
                            rsc->id, task, services_ocf_exitcode_str(rc), rc);
                clear_bit(rsc->flags, pe_rsc_managed);
                set_bit(rsc->flags, pe_rsc_block);
            }
            result = PCMK_LRM_OP_ERROR_HARD;
            break;

        default:
            if (result == PCMK_LRM_OP_DONE) {
                crm_info("Treating %s (rc=%d) on %s as an ERROR",
                         key, rc, node->details->uname);
                result = PCMK_LRM_OP_ERROR;
            }
    }

    return result;
}

static bool check_operation_expiry(resource_t *rsc, node_t *node, int rc, xmlNode *xml_op, pe_working_set_t * data_set)
{
    bool expired = FALSE;
    time_t last_failure = 0;
    guint interval_ms = 0;
    int failure_timeout = rsc->failure_timeout;
    const char *key = get_op_key(xml_op);
    const char *task = crm_element_value(xml_op, XML_LRM_ATTR_TASK);
    const char *clear_reason = NULL;

    crm_element_value_ms(xml_op, XML_LRM_ATTR_INTERVAL_MS, &interval_ms);

    /* clearing recurring monitor operation failures automatically
     * needs to be carefully considered */
    if ((interval_ms != 0) && safe_str_eq(task, "monitor")) {

        /* TODO, in the future we should consider not clearing recurring monitor
         * op failures unless the last action for a resource was a "stop" action.
         * otherwise it is possible that clearing the monitor failure will result
         * in the resource being in an undeterministic state.
         *
         * For now we handle this potential undeterministic condition for remote
         * node connection resources by not clearing a recurring monitor op failure
         * until after the node has been fenced. */

        if (is_set(data_set->flags, pe_flag_stonith_enabled)
            && rsc->remote_reconnect_ms) {

            node_t *remote_node = pe_find_node(data_set->nodes, rsc->id);
            if (remote_node && remote_node->details->remote_was_fenced == 0) {
                if (strstr(ID(xml_op), "last_failure")) {
                    crm_info("Waiting to clear monitor failure for remote node %s until fencing has occurred", rsc->id); 
                }
                /* disabling failure timeout for this operation because we believe
                 * fencing of the remote node should occur first. */ 
                failure_timeout = 0;
            }
        }
    }

    if (failure_timeout > 0) {
        int last_run = 0;

        if (crm_element_value_int(xml_op, XML_RSC_OP_LAST_CHANGE, &last_run) == 0) {
            time_t now = get_effective_time(data_set);

            if (now > (last_run + failure_timeout)) {
                expired = TRUE;
            }
        }
    }

    if (expired) {
        if (failure_timeout > 0) {
            if (pe_get_failcount(node, rsc, &last_failure, pe_fc_default,
                                 xml_op, data_set)) {

                if (pe_get_failcount(node, rsc, &last_failure, pe_fc_effective,
                                     xml_op, data_set) == 0) {
                    clear_reason = "it expired";
                } else {
                    expired = FALSE;
                }

            } else if (rsc->remote_reconnect_ms
                       && strstr(ID(xml_op), "last_failure")) {
                /* always clear last failure when reconnect interval is set */
                clear_reason = "reconnect interval is set";
            }
        }

    } else if (strstr(ID(xml_op), "last_failure") &&
               ((strcmp(task, "start") == 0) || (strcmp(task, "monitor") == 0))) {

        op_digest_cache_t *digest_data = NULL;

        digest_data = rsc_action_digest_cmp(rsc, xml_op, node, data_set);

        if (digest_data->rc == RSC_DIGEST_UNKNOWN) {
            crm_trace("rsc op %s/%s on node %s does not have a op digest to compare against", rsc->id,
                      key, node->details->id);
        } else if(container_fix_remote_addr(rsc) && digest_data->rc != RSC_DIGEST_MATCH) {
            // We can't sanely check the changing 'addr' attribute. Yet
            crm_trace("Ignoring rsc op %s/%s on node %s", rsc->id, key, node->details->id);

        } else if (digest_data->rc != RSC_DIGEST_MATCH) {
            clear_reason = "resource parameters have changed";
        }
    }

    if (clear_reason != NULL) {
        char *key = generate_op_key(rsc->id, CRM_OP_CLEAR_FAILCOUNT, 0);
        action_t *clear_op = custom_action(rsc, key, CRM_OP_CLEAR_FAILCOUNT,
                                           node, FALSE, TRUE, data_set);

        add_hash_param(clear_op->meta, XML_ATTR_TE_NOWAIT, XML_BOOLEAN_TRUE);

        crm_notice("Clearing failure of %s on %s because %s " CRM_XS " %s",
                   rsc->id, node->details->uname, clear_reason, clear_op->uuid);
    }

    if (expired && (interval_ms == 0) && safe_str_eq(task, CRMD_ACTION_STATUS)) {
        switch(rc) {
            case PCMK_OCF_OK:
            case PCMK_OCF_NOT_RUNNING:
            case PCMK_OCF_RUNNING_MASTER:
            case PCMK_OCF_DEGRADED:
            case PCMK_OCF_DEGRADED_MASTER:
                /* Don't expire probes that return these values */ 
                expired = FALSE;
                break;
        }
    }
    
    return expired;
}

int get_target_rc(xmlNode *xml_op)
{
    int dummy = 0;
    int target_rc = 0;
    char *dummy_string = NULL;
    const char *key = crm_element_value(xml_op, XML_ATTR_TRANSITION_KEY);
    if (key == NULL) {
        return -1;
    }

    decode_transition_key(key, &dummy_string, &dummy, &dummy, &target_rc);
    free(dummy_string);

    return target_rc;
}

static enum action_fail_response
get_action_on_fail(resource_t *rsc, const char *key, const char *task, pe_working_set_t * data_set) 
{
    int result = action_fail_recover;
    action_t *action = custom_action(rsc, strdup(key), task, NULL, TRUE, FALSE, data_set);

    result = action->on_fail;
    pe_free_action(action);

    return result;
}

static void
update_resource_state(resource_t * rsc, node_t * node, xmlNode * xml_op, const char * task, int rc,
                      xmlNode * last_failure, enum action_fail_response * on_fail, pe_working_set_t * data_set)
{
    gboolean clear_past_failure = FALSE;

    CRM_ASSERT(rsc);
    CRM_ASSERT(xml_op);

    if (rc == PCMK_OCF_NOT_RUNNING) {
        clear_past_failure = TRUE;

    } else if (rc == PCMK_OCF_NOT_INSTALLED) {
        rsc->role = RSC_ROLE_STOPPED;

    } else if (safe_str_eq(task, CRMD_ACTION_STATUS)) {
        if (last_failure) {
            const char *op_key = get_op_key(xml_op);
            const char *last_failure_key = get_op_key(last_failure);

            if (safe_str_eq(op_key, last_failure_key)) {
                clear_past_failure = TRUE;
            }
        }

        if (rsc->role < RSC_ROLE_STARTED) {
            set_active(rsc);
        }

    } else if (safe_str_eq(task, CRMD_ACTION_START)) {
        rsc->role = RSC_ROLE_STARTED;
        clear_past_failure = TRUE;

    } else if (safe_str_eq(task, CRMD_ACTION_STOP)) {
        rsc->role = RSC_ROLE_STOPPED;
        clear_past_failure = TRUE;

    } else if (safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
        rsc->role = RSC_ROLE_MASTER;
        clear_past_failure = TRUE;

    } else if (safe_str_eq(task, CRMD_ACTION_DEMOTE)) {
        /* Demote from Master does not clear an error */
        rsc->role = RSC_ROLE_SLAVE;

    } else if (safe_str_eq(task, CRMD_ACTION_MIGRATED)) {
        rsc->role = RSC_ROLE_STARTED;
        clear_past_failure = TRUE;

    } else if (safe_str_eq(task, CRMD_ACTION_MIGRATE)) {
        unpack_rsc_migration(rsc, node, xml_op, data_set);

    } else if (rsc->role < RSC_ROLE_STARTED) {
        pe_rsc_trace(rsc, "%s active on %s", rsc->id, node->details->uname);
        set_active(rsc);
    }

    /* clear any previous failure actions */
    if (clear_past_failure) {
        switch (*on_fail) {
            case action_fail_stop:
            case action_fail_fence:
            case action_fail_migrate:
            case action_fail_standby:
                pe_rsc_trace(rsc, "%s.%s is not cleared by a completed stop",
                             rsc->id, fail2text(*on_fail));
                break;

            case action_fail_block:
            case action_fail_ignore:
            case action_fail_recover:
            case action_fail_restart_container:
                *on_fail = action_fail_ignore;
                rsc->next_role = RSC_ROLE_UNKNOWN;
                break;
            case action_fail_reset_remote:
                if (rsc->remote_reconnect_ms == 0) {
                    /* when reconnect delay is not in use, the connection is allowed
                     * to start again after the remote node is fenced and completely
                     * stopped. Otherwise, with reconnect delay we wait for the failure
                     * to be cleared entirely before reconnected can be attempted. */ 
                    *on_fail = action_fail_ignore;
                    rsc->next_role = RSC_ROLE_UNKNOWN;
                }
                break;
        }
    }
}


gboolean
unpack_rsc_op(resource_t * rsc, node_t * node, xmlNode * xml_op, xmlNode ** last_failure,
              enum action_fail_response * on_fail, pe_working_set_t * data_set)
{
    int task_id = 0;

    const char *key = NULL;
    const char *task = NULL;
    const char *task_key = NULL;

    int rc = 0;
    int status = PCMK_LRM_OP_UNKNOWN;
    int target_rc = get_target_rc(xml_op);
    guint interval_ms = 0;

    gboolean expired = FALSE;
    resource_t *parent = rsc;
    enum action_fail_response failure_strategy = action_fail_recover;

    CRM_CHECK(rsc != NULL, return FALSE);
    CRM_CHECK(node != NULL, return FALSE);
    CRM_CHECK(xml_op != NULL, return FALSE);

    task_key = get_op_key(xml_op);

    task = crm_element_value(xml_op, XML_LRM_ATTR_TASK);
    key = crm_element_value(xml_op, XML_ATTR_TRANSITION_KEY);

    crm_element_value_int(xml_op, XML_LRM_ATTR_RC, &rc);
    crm_element_value_int(xml_op, XML_LRM_ATTR_CALLID, &task_id);
    crm_element_value_int(xml_op, XML_LRM_ATTR_OPSTATUS, &status);
    crm_element_value_ms(xml_op, XML_LRM_ATTR_INTERVAL_MS, &interval_ms);

    CRM_CHECK(task != NULL, return FALSE);
    CRM_CHECK(status <= PCMK_LRM_OP_NOT_INSTALLED, return FALSE);
    CRM_CHECK(status >= PCMK_LRM_OP_PENDING, return FALSE);

    if (safe_str_eq(task, CRMD_ACTION_NOTIFY) ||
        safe_str_eq(task, CRMD_ACTION_METADATA)) {
        /* safe to ignore these */
        return TRUE;
    }

    if (is_not_set(rsc->flags, pe_rsc_unique)) {
        parent = uber_parent(rsc);
    }

    pe_rsc_trace(rsc, "Unpacking task %s/%s (call_id=%d, status=%d, rc=%d) on %s (role=%s)",
                 task_key, task, task_id, status, rc, node->details->uname, role2text(rsc->role));

    if (node->details->unclean) {
        pe_rsc_trace(rsc, "Node %s (where %s is running) is unclean."
                     " Further action depends on the value of the stop's on-fail attribute",
                     node->details->uname, rsc->id);
    }

    if (status == PCMK_LRM_OP_ERROR) {
        /* Older versions set this if rc != 0 but it's up to us to decide */
        status = PCMK_LRM_OP_DONE;
    }

    if(status != PCMK_LRM_OP_NOT_INSTALLED) {
        expired = check_operation_expiry(rsc, node, rc, xml_op, data_set);
    }

    /* Degraded results are informational only, re-map them to their error-free equivalents */
    if (rc == PCMK_OCF_DEGRADED && safe_str_eq(task, CRMD_ACTION_STATUS)) {
        rc = PCMK_OCF_OK;

        /* Add them to the failed list to highlight them for the user */
        if ((node->details->shutdown == FALSE) || (node->details->online == TRUE)) {
            crm_trace("Remapping %d to %d", PCMK_OCF_DEGRADED, PCMK_OCF_OK);
            record_failed_op(xml_op, node, rsc, data_set);
        }

    } else if (rc == PCMK_OCF_DEGRADED_MASTER && safe_str_eq(task, CRMD_ACTION_STATUS)) {
        rc = PCMK_OCF_RUNNING_MASTER;

        /* Add them to the failed list to highlight them for the user */
        if ((node->details->shutdown == FALSE) || (node->details->online == TRUE)) {
            crm_trace("Remapping %d to %d", PCMK_OCF_DEGRADED_MASTER, PCMK_OCF_RUNNING_MASTER);
            record_failed_op(xml_op, node, rsc, data_set);
        }
    }

    if (expired && target_rc != rc) {
        const char *magic = crm_element_value(xml_op, XML_ATTR_TRANSITION_MAGIC);

        pe_rsc_debug(rsc, "Expired operation '%s' on %s returned '%s' (%d) instead of the expected value: '%s' (%d)",
                     key, node->details->uname,
                     services_ocf_exitcode_str(rc), rc,
                     services_ocf_exitcode_str(target_rc), target_rc);

        if (interval_ms == 0) {
            crm_notice("Ignoring expired calculated failure %s (rc=%d, magic=%s) on %s",
                       task_key, rc, magic, node->details->uname);
            goto done;

        } else if(node->details->online && node->details->unclean == FALSE) {
            crm_notice("Re-initiated expired calculated failure %s (rc=%d, magic=%s) on %s",
                       task_key, rc, magic, node->details->uname);
            /* This is SO horrible, but we don't have access to CancelXmlOp() yet */
            crm_xml_add(xml_op, XML_LRM_ATTR_RESTART_DIGEST, "calculated-failure-timeout");
            goto done;
        }
    }

    if(status == PCMK_LRM_OP_DONE || status == PCMK_LRM_OP_ERROR) {
        status = determine_op_status(rsc, rc, target_rc, node, xml_op, on_fail, data_set);
    }

    pe_rsc_trace(rsc, "Handling status: %d", status);
    switch (status) {
        case PCMK_LRM_OP_CANCELLED:
            /* do nothing?? */
            pe_err("Don't know what to do for cancelled ops yet");
            break;

        case PCMK_LRM_OP_PENDING:
            if (safe_str_eq(task, CRMD_ACTION_START)) {
                set_bit(rsc->flags, pe_rsc_start_pending);
                set_active(rsc);

            } else if (safe_str_eq(task, CRMD_ACTION_PROMOTE)) {
                rsc->role = RSC_ROLE_MASTER;

            } else if (safe_str_eq(task, CRMD_ACTION_MIGRATE) && node->details->unclean) {
                /* If a pending migrate_to action is out on a unclean node,
                 * we have to force the stop action on the target. */
                const char *migrate_target = crm_element_value(xml_op, XML_LRM_ATTR_MIGRATE_TARGET);
                node_t *target = pe_find_node(data_set->nodes, migrate_target);
                if (target) {
                    stop_action(rsc, target, FALSE);
                }
            }

            if (rsc->pending_task == NULL) {
                if (safe_str_eq(task, CRMD_ACTION_STATUS) && (interval_ms == 0)) {
                    /* Pending probes are not printed, even if pending
                     * operations are requested. If someone ever requests that
                     * behavior, uncomment this and the corresponding part of
                     * native.c:native_pending_task().
                     */
                    /*rsc->pending_task = strdup("probe");*/

                } else {
                    rsc->pending_task = strdup(task);
                }
            }
            break;

        case PCMK_LRM_OP_DONE:
            pe_rsc_trace(rsc, "%s/%s completed on %s", rsc->id, task, node->details->uname);
            update_resource_state(rsc, node, xml_op, task, rc, *last_failure, on_fail, data_set);
            break;

        case PCMK_LRM_OP_NOT_INSTALLED:
            failure_strategy = get_action_on_fail(rsc, task_key, task, data_set);
            if (failure_strategy == action_fail_ignore) {
                crm_warn("Cannot ignore failed %s (status=%d, rc=%d) on %s: "
                         "Resource agent doesn't exist",
                         task_key, status, rc, node->details->uname);
                /* Also for printing it as "FAILED" by marking it as pe_rsc_failed later */
                *on_fail = action_fail_migrate;
            }
            resource_location(parent, node, -INFINITY, "hard-error", data_set);
            unpack_rsc_op_failure(rsc, node, rc, xml_op, last_failure, on_fail, data_set);
            break;

        case PCMK_LRM_OP_ERROR:
        case PCMK_LRM_OP_ERROR_HARD:
        case PCMK_LRM_OP_ERROR_FATAL:
        case PCMK_LRM_OP_TIMEOUT:
        case PCMK_LRM_OP_NOTSUPPORTED:

            failure_strategy = get_action_on_fail(rsc, task_key, task, data_set);
            if ((failure_strategy == action_fail_ignore)
                || (failure_strategy == action_fail_restart_container
                    && safe_str_eq(task, CRMD_ACTION_STOP))) {

                crm_warn("Pretending the failure of %s (rc=%d) on %s succeeded",
                         task_key, rc, node->details->uname);

                update_resource_state(rsc, node, xml_op, task, target_rc, *last_failure, on_fail, data_set);
                crm_xml_add(xml_op, XML_ATTR_UNAME, node->details->uname);
                set_bit(rsc->flags, pe_rsc_failure_ignored);

                record_failed_op(xml_op, node, rsc, data_set);

                if (failure_strategy == action_fail_restart_container && *on_fail <= action_fail_recover) {
                    *on_fail = failure_strategy;
                }

            } else {
                unpack_rsc_op_failure(rsc, node, rc, xml_op, last_failure, on_fail, data_set);

                if(status == PCMK_LRM_OP_ERROR_HARD) {
                    do_crm_log(rc != PCMK_OCF_NOT_INSTALLED?LOG_ERR:LOG_NOTICE,
                               "Preventing %s from re-starting on %s: operation %s failed '%s' (%d)",
                               parent->id, node->details->uname,
                               task, services_ocf_exitcode_str(rc), rc);

                    resource_location(parent, node, -INFINITY, "hard-error", data_set);

                } else if(status == PCMK_LRM_OP_ERROR_FATAL) {
                    crm_err("Preventing %s from re-starting anywhere: operation %s failed '%s' (%d)",
                            parent->id, task, services_ocf_exitcode_str(rc), rc);

                    resource_location(parent, NULL, -INFINITY, "fatal-error", data_set);
                }
            }
            break;
    }

  done:
    pe_rsc_trace(rsc, "Resource %s after %s: role=%s, next=%s", rsc->id, task, role2text(rsc->role), role2text(rsc->next_role));
    return TRUE;
}

gboolean
add_node_attrs(xmlNode * xml_obj, node_t * node, gboolean overwrite, pe_working_set_t * data_set)
{
    const char *cluster_name = NULL;

    g_hash_table_insert(node->details->attrs,
                        strdup(CRM_ATTR_UNAME), strdup(node->details->uname));

    g_hash_table_insert(node->details->attrs, strdup(CRM_ATTR_ID),
                        strdup(node->details->id));
    if (safe_str_eq(node->details->id, data_set->dc_uuid)) {
        data_set->dc_node = node;
        node->details->is_dc = TRUE;
        g_hash_table_insert(node->details->attrs,
                            strdup(CRM_ATTR_IS_DC), strdup(XML_BOOLEAN_TRUE));
    } else {
        g_hash_table_insert(node->details->attrs,
                            strdup(CRM_ATTR_IS_DC), strdup(XML_BOOLEAN_FALSE));
    }

    cluster_name = g_hash_table_lookup(data_set->config_hash, "cluster-name");
    if (cluster_name) {
        g_hash_table_insert(node->details->attrs, strdup(CRM_ATTR_CLUSTER_NAME),
                            strdup(cluster_name));
    }

    unpack_instance_attributes(data_set->input, xml_obj, XML_TAG_ATTR_SETS, NULL,
                               node->details->attrs, NULL, overwrite, data_set->now);

    if (pe_node_attribute_raw(node, CRM_ATTR_SITE_NAME) == NULL) {
        const char *site_name = pe_node_attribute_raw(node, "site-name");

        if (site_name) {
            g_hash_table_insert(node->details->attrs,
                                strdup(CRM_ATTR_SITE_NAME),
                                strdup(site_name));

        } else if (cluster_name) {
            /* Default to cluster-name if unset */
            g_hash_table_insert(node->details->attrs,
                                strdup(CRM_ATTR_SITE_NAME),
                                strdup(cluster_name));
        }
    }
    return TRUE;
}

static GListPtr
extract_operations(const char *node, const char *rsc, xmlNode * rsc_entry, gboolean active_filter)
{
    int counter = -1;
    int stop_index = -1;
    int start_index = -1;

    xmlNode *rsc_op = NULL;

    GListPtr gIter = NULL;
    GListPtr op_list = NULL;
    GListPtr sorted_op_list = NULL;

    /* extract operations */
    op_list = NULL;
    sorted_op_list = NULL;

    for (rsc_op = __xml_first_child(rsc_entry); rsc_op != NULL; rsc_op = __xml_next_element(rsc_op)) {
        if (crm_str_eq((const char *)rsc_op->name, XML_LRM_TAG_RSC_OP, TRUE)) {
            crm_xml_add(rsc_op, "resource", rsc);
            crm_xml_add(rsc_op, XML_ATTR_UNAME, node);
            op_list = g_list_prepend(op_list, rsc_op);
        }
    }

    if (op_list == NULL) {
        /* if there are no operations, there is nothing to do */
        return NULL;
    }

    sorted_op_list = g_list_sort(op_list, sort_op_by_callid);

    /* create active recurring operations as optional */
    if (active_filter == FALSE) {
        return sorted_op_list;
    }

    op_list = NULL;

    calculate_active_ops(sorted_op_list, &start_index, &stop_index);

    for (gIter = sorted_op_list; gIter != NULL; gIter = gIter->next) {
        xmlNode *rsc_op = (xmlNode *) gIter->data;

        counter++;

        if (start_index < stop_index) {
            crm_trace("Skipping %s: not active", ID(rsc_entry));
            break;

        } else if (counter < start_index) {
            crm_trace("Skipping %s: old", ID(rsc_op));
            continue;
        }
        op_list = g_list_append(op_list, rsc_op);
    }

    g_list_free(sorted_op_list);
    return op_list;
}

GListPtr
find_operations(const char *rsc, const char *node, gboolean active_filter,
                pe_working_set_t * data_set)
{
    GListPtr output = NULL;
    GListPtr intermediate = NULL;

    xmlNode *tmp = NULL;
    xmlNode *status = find_xml_node(data_set->input, XML_CIB_TAG_STATUS, TRUE);

    node_t *this_node = NULL;

    xmlNode *node_state = NULL;

    for (node_state = __xml_first_child(status); node_state != NULL;
         node_state = __xml_next_element(node_state)) {

        if (crm_str_eq((const char *)node_state->name, XML_CIB_TAG_STATE, TRUE)) {
            const char *uname = crm_element_value(node_state, XML_ATTR_UNAME);

            if (node != NULL && safe_str_neq(uname, node)) {
                continue;
            }

            this_node = pe_find_node(data_set->nodes, uname);
            if(this_node == NULL) {
                CRM_LOG_ASSERT(this_node != NULL);
                continue;

            } else if (is_remote_node(this_node)) {
                determine_remote_online_status(data_set, this_node);

            } else {
                determine_online_status(node_state, this_node, data_set);
            }

            if (this_node->details->online || is_set(data_set->flags, pe_flag_stonith_enabled)) {
                /* offline nodes run no resources...
                 * unless stonith is enabled in which case we need to
                 *   make sure rsc start events happen after the stonith
                 */
                xmlNode *lrm_rsc = NULL;

                tmp = find_xml_node(node_state, XML_CIB_TAG_LRM, FALSE);
                tmp = find_xml_node(tmp, XML_LRM_TAG_RESOURCES, FALSE);

                for (lrm_rsc = __xml_first_child(tmp); lrm_rsc != NULL;
                     lrm_rsc = __xml_next_element(lrm_rsc)) {
                    if (crm_str_eq((const char *)lrm_rsc->name, XML_LRM_TAG_RESOURCE, TRUE)) {

                        const char *rsc_id = crm_element_value(lrm_rsc, XML_ATTR_ID);

                        if (rsc != NULL && safe_str_neq(rsc_id, rsc)) {
                            continue;
                        }

                        intermediate = extract_operations(uname, rsc_id, lrm_rsc, active_filter);
                        output = g_list_concat(output, intermediate);
                    }
                }
            }
        }
    }

    return output;
}
