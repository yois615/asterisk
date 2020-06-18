/*
 * asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2014, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*** MODULEINFO
	<depend>pjproject</depend>
	<depend>res_pjsip</depend>
	<depend>res_pjsip_pubsub</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_simple.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/callerid.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_pubsub.h"
#include "asterisk/res_pjsip_presence_xml.h"
#include "asterisk/res_pjsip_body_generator_types.h"

/*! \brief Structure which contains dialog-info+xml state information */
struct dialog_info_xml_state {
	/*! \brief Version to place into the next NOTIFY */
	unsigned int version;
};

/*! \brief Destructor for dialog-info+xml information */
static void dialog_info_xml_state_destroy(void *obj)
{
	ast_free(obj);
}

/*! \brief Datastore for attaching dialog-info+xml state information */
static const struct ast_datastore_info dialog_info_xml_datastore = {
	.type = "dialog-info+xml",
	.destroy = dialog_info_xml_state_destroy,
};

static void *dialog_info_allocate_body(void *data)
{
	struct ast_sip_exten_state_data *state_data = data;

	return ast_sip_presence_xml_create_node(state_data->pool, NULL, "dialog-info");
}

static struct ast_channel *find_ringing_channel(struct ao2_container *device_state_info)
{
        struct ao2_iterator citer;
        struct ast_device_state_info *device_state;
        struct ast_channel *c = NULL;
        struct timeval tv = {0,};

        /* iterate ringing devices and get the oldest of all causing channels */
        citer = ao2_iterator_init(device_state_info, 0);
        for (; (device_state = ao2_iterator_next(&citer)); ao2_ref(device_state, -1)) {
                if (!device_state->causing_channel || (device_state->device_state != AST_DEVICE_RINGING &&
                    device_state->device_state != AST_DEVICE_RINGINUSE)) {
                        continue;
                }
                ast_channel_lock(device_state->causing_channel);
                if (ast_tvzero(tv) || ast_tvcmp(ast_channel_creationtime(device_state->causing_channel), tv) < 0) {
                        c = device_state->causing_channel;
                        tv = ast_channel_creationtime(c);
                }
                ast_channel_unlock(device_state->causing_channel);
        }
        ao2_iterator_destroy(&citer);
        return c ? ast_channel_ref(c) : NULL;
}
/*
static int get_domain_from_uri(pjsip_uri *from, char *domain, size_t domain_size)
{
        pjsip_sip_uri *sip_from;

        if (from == NULL || 
		(!PJSIP_URI_SCHEME_IS_SIP(from) && !PJSIP_URI_SCHEME_IS_SIPS(from))) {
                return -1;
        }
        sip_from = (pjsip_sip_uri *) pjsip_uri_get_uri(from);
        ast_copy_pj_str(domain, &sip_from->host, domain_size);
        return 1;
}
*/

static int dialog_info_generate_body_content(void *body, void *data)
{
	pj_xml_node *dialog_info = body, *dialog, *state, *remote_node, *local_node, *remote_identity_node, *remote_target_node, *local_identity_node, *local_target_node;
	struct ast_datastore *datastore;
	struct dialog_info_xml_state *datastore_state;
	struct ast_sip_exten_state_data *state_data = data;
	char *local = ast_strdupa(state_data->local), *stripped, *statestring = NULL;
	char *remote = ast_strdupa(state_data->remote), *stripped_remote;
	char *pidfstate = NULL, *pidfnote = NULL;
	enum ast_sip_pidf_state local_state;
	char version_str[32], remote_target[PJSIP_MAX_URL_SIZE], sanitized[PJSIP_MAX_URL_SIZE], sanitized_remote[PJSIP_MAX_URL_SIZE];
	struct ast_sip_endpoint *endpoint = NULL;
	unsigned int notify_early_inuse_ringing = 0;
	struct ast_channel *callee;
	const char *from_domain = NULL;
	static char *invalid = "anonymous.invalid";
	

	if (!local || !remote || !state_data->datastores) {
		return -1;
	}

	datastore = ast_datastores_find(state_data->datastores, "dialog-info+xml");
	if (!datastore) {
		const struct ast_json *version_json = NULL;

		datastore = ast_datastores_alloc_datastore(&dialog_info_xml_datastore, "dialog-info+xml");
		if (!datastore) {
			return -1;
		}

		datastore->data = ast_calloc(1, sizeof(struct dialog_info_xml_state));
		if (!datastore->data || ast_datastores_add(state_data->datastores, datastore)) {
			ao2_ref(datastore, -1);
			return -1;
		}
		datastore_state = datastore->data;

		if (state_data->sub) {
			version_json = ast_sip_subscription_get_persistence_data(state_data->sub);
		}

		if (version_json) {
			datastore_state->version = ast_json_integer_get(version_json);
			datastore_state->version++;
		} else {
			datastore_state->version = 0;
		}
	} else {
		datastore_state = datastore->data;
		datastore_state->version++;
	}

	stripped = ast_strip_quoted(local, "<", ">");
	stripped_remote = ast_strip_quoted(remote, "<", ">");
	ast_sip_sanitize_xml(stripped, sanitized, sizeof(sanitized));
	ast_sip_sanitize_xml(stripped_remote, sanitized_remote, sizeof(sanitized_remote));

	if (state_data->sub && (endpoint = ast_sip_subscription_get_endpoint(state_data->sub))) {
	    notify_early_inuse_ringing = endpoint->notify_early_inuse_ringing;
		from_domain = endpoint ? (!ast_strlen_zero(endpoint->fromdomain) ? endpoint->fromdomain : invalid) : NULL;
	    ao2_cleanup(endpoint);
	}
	ast_sip_presence_exten_state_to_str(state_data->exten_state, &statestring,
			&pidfstate, &pidfnote, &local_state, notify_early_inuse_ringing);

	ast_sip_presence_xml_create_attr(state_data->pool, dialog_info, "xmlns", "urn:ietf:params:xml:ns:dialog-info");

	snprintf(version_str, sizeof(version_str), "%u", datastore_state->version);
	ast_sip_presence_xml_create_attr(state_data->pool, dialog_info, "version", version_str);

	if (state_data->sub) {
		ast_sip_subscription_set_persistence_data(state_data->sub, ast_json_integer_create(datastore_state->version));
	}

	ast_sip_presence_xml_create_attr(state_data->pool, dialog_info, "state", "full");
	ast_sip_presence_xml_create_attr(state_data->pool, dialog_info, "entity", sanitized);

	dialog = ast_sip_presence_xml_create_node(state_data->pool, dialog_info, "dialog");
	ast_sip_presence_xml_create_attr(state_data->pool, dialog, "id", state_data->exten);
	if (!ast_strlen_zero(statestring) && !strcmp(statestring, "early")) {
		ast_sip_presence_xml_create_attr(state_data->pool, dialog, "direction", "recipient");

		callee = find_ringing_channel(state_data->device_state_info);

		if (callee) {
			remote_node = ast_sip_presence_xml_create_node(state_data->pool, dialog, "remote");
			remote_identity_node = ast_sip_presence_xml_create_node(state_data->pool, remote_node, "identity");
			remote_target_node = ast_sip_presence_xml_create_node(state_data->pool, remote_node, "target");

			static char *anonymous = "anonymous";
			char *cid_num;
			char *cid_name;
			char *connected_num;
			// pjsip_uri *parsed_uri;
			int need;
			int cid_num_restricted, connected_num_restricted;
        		// char from_domain[255 + 1];

			ast_log(LOG_WARNING, "From Domain remote URI domain '%s'\n", sanitized_remote);
			/*
			parsed_uri = pjsip_parse_uri(state_data->pool, sanitized_remote, sizeof(sanitized_remote), 0);
			if (!get_domain_from_uri(parsed_uri, from_domain, sizeof(from_domain))) {
				ast_log(LOG_WARNING, "could not determine remote URI\n");
				return -1;
			} else {
				ast_log(LOG_WARNING, "Remote URI domain '%s'\n", from_domain);
			}
			*/

			ast_channel_lock(callee);

			cid_num_restricted = (ast_channel_caller(callee)->id.number.presentation &
						   AST_PRES_RESTRICTION) == AST_PRES_RESTRICTED;
			cid_num = S_COR(ast_channel_caller(callee)->id.number.valid,
					S_COR(cid_num_restricted, anonymous,
					      ast_channel_caller(callee)->id.number.str), "");

			cid_name = S_COR(ast_channel_connected(callee)->id.name.valid,
					     S_COR((ast_channel_connected(callee)->id.name.presentation &
						     AST_PRES_RESTRICTION) == AST_PRES_RESTRICTED, anonymous,
						    ast_channel_connected(callee)->id.name.str), "");

			need = strlen(cid_num) + (cid_num_restricted ? strlen(invalid) :
						  strlen(from_domain)) + sizeof("sip:@");

			connected_num_restricted = (ast_channel_connected(callee)->id.number.presentation &
						    AST_PRES_RESTRICTION) == AST_PRES_RESTRICTED;
			connected_num = S_COR(ast_channel_connected(callee)->id.number.valid,
					      S_COR(connected_num_restricted, anonymous,
						    ast_channel_connected(callee)->id.number.str), "");

			need = strlen(connected_num) + (connected_num_restricted ? strlen(invalid) :
							strlen(from_domain)) + sizeof("sip:@");
			// remote_target = ast_alloca(need);

			snprintf(remote_target, need, "sip:%s@%s", connected_num,
				 connected_num_restricted ? invalid : from_domain);

			ast_channel_unlock(callee);
			callee = ast_channel_unref(callee);

			pj_strdup2(state_data->pool, &remote_identity_node->content, remote_target);
			ast_sip_presence_xml_create_attr(state_data->pool, remote_identity_node, "display", cid_name);
			ast_sip_presence_xml_create_attr(state_data->pool, remote_target_node, "uri", remote_target);
		}

		local_node = ast_sip_presence_xml_create_node(state_data->pool, dialog, "local");
		local_identity_node = ast_sip_presence_xml_create_node(state_data->pool, local_node, "identity");
		local_target_node = ast_sip_presence_xml_create_node(state_data->pool, local_node, "target");

		pj_strdup2(state_data->pool, &local_identity_node->content, sanitized);
		ast_sip_presence_xml_create_attr(state_data->pool, local_target_node, "uri", sanitized);
	}

	state = ast_sip_presence_xml_create_node(state_data->pool, dialog, "state");
	pj_strdup2(state_data->pool, &state->content, statestring);

	if (state_data->exten_state == AST_EXTENSION_ONHOLD) {
		pj_xml_node *local_node, *target, *param;

		local_node = ast_sip_presence_xml_create_node(state_data->pool, dialog, "local");
		target = ast_sip_presence_xml_create_node(state_data->pool, local_node, "target");
		ast_sip_presence_xml_create_attr(state_data->pool, target, "uri", sanitized);
		param = ast_sip_presence_xml_create_node(state_data->pool, target, "param");
		ast_sip_presence_xml_create_attr(state_data->pool, param, "pname", "+sip.rendering");
		ast_sip_presence_xml_create_attr(state_data->pool, param, "pvalue", "no");
	}

	ao2_ref(datastore, -1);

	return 0;
}

/* The maximum number of times the ast_str() for the body text can grow before we declare an XML body
 * too large to send.
 */
#define MAX_STRING_GROWTHS 6

static void dialog_info_to_string(void *body, struct ast_str **str)
{
	pj_xml_node *dialog_info = body;
	int growths = 0;
	int size;

	do {
		size = pj_xml_print(dialog_info, ast_str_buffer(*str), ast_str_size(*str) - 1, PJ_TRUE);
		if (size <= AST_PJSIP_XML_PROLOG_LEN) {
			ast_str_make_space(str, ast_str_size(*str) * 2);
			++growths;
		}
	} while (size <= AST_PJSIP_XML_PROLOG_LEN && growths < MAX_STRING_GROWTHS);
	if (size <= AST_PJSIP_XML_PROLOG_LEN) {
		ast_log(LOG_WARNING, "dialog-info+xml body text too large\n");
		return;
	}

	*(ast_str_buffer(*str) + size) = '\0';
	ast_str_update(*str);
}

static struct ast_sip_pubsub_body_generator dialog_info_body_generator = {
	.type = "application",
	.subtype = "dialog-info+xml",
	.body_type = AST_SIP_EXTEN_STATE_DATA,
	.allocate_body = dialog_info_allocate_body,
	.generate_body_content = dialog_info_generate_body_content,
	.to_string = dialog_info_to_string,
	/* No need for a destroy_body callback since we use a pool */
};

static int load_module(void)
{
	if (ast_sip_pubsub_register_body_generator(&dialog_info_body_generator)) {
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_sip_pubsub_unregister_body_generator(&dialog_info_body_generator);
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "PJSIP Extension State Dialog Info+XML Provider",
	.support_level = AST_MODULE_SUPPORT_CORE,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_pubsub",
);
