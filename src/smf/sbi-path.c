/*
 * Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "context.h"
#include "sbi-path.h"

static int server_cb(ogs_sbi_server_t *server,
        ogs_sbi_session_t *session, ogs_sbi_request_t *request)
{
    smf_event_t *e = NULL;
    int rv;

    ogs_assert(session);
    ogs_assert(request);

    e = smf_event_new(SMF_EVT_SBI_SERVER);
    ogs_assert(e);

    e->sbi.server = server;
    e->sbi.session = session;
    e->sbi.request = request;

    rv = ogs_queue_push(smf_self()->queue, e);
    if (rv != OGS_OK) {
        ogs_warn("ogs_queue_push() failed:%d", (int)rv);
        smf_event_free(e);
        return OGS_ERROR;
    }

    return OGS_OK;
}

static int client_cb(ogs_sbi_response_t *response, void *data)
{
    smf_event_t *e = NULL;
    int rv;

    ogs_assert(response);

    e = smf_event_new(SMF_EVT_SBI_CLIENT);
    ogs_assert(e);
    e->sbi.response = response;
    e->sbi.data = data;

    rv = ogs_queue_push(smf_self()->queue, e);
    if (rv != OGS_OK) {
        ogs_warn("ogs_queue_push() failed:%d", (int)rv);
        smf_event_free(e);
        return OGS_ERROR;
    }

    return OGS_OK;
}

int smf_sbi_open(void)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL;

    ogs_sbi_server_start_all(server_cb);

    ogs_list_for_each(&ogs_sbi_self()->nf_instance_list, nf_instance) {
        ogs_sbi_nf_service_t *service = NULL;

        ogs_sbi_nf_instance_build_default(nf_instance, smf_self()->nf_type);

        service = ogs_sbi_nf_service_build_default(nf_instance,
                (char*)OGS_SBI_SERVICE_NAME_NSMF_PDUSESSION);
        ogs_assert(service);
        ogs_sbi_nf_service_add_version(service, (char*)OGS_SBI_API_V1,
                (char*)OGS_SBI_API_V1_0_0, NULL);

        smf_nf_fsm_init(nf_instance);
        smf_sbi_setup_client_callback(nf_instance);
    }

    return OGS_OK;
}

void smf_sbi_close(void)
{
    ogs_sbi_server_stop_all();
}

void smf_sbi_setup_client_callback(ogs_sbi_nf_instance_t *nf_instance)
{
    ogs_sbi_client_t *client = NULL;
    ogs_sbi_nf_service_t *nf_service = NULL;
    ogs_assert(nf_instance);

    client = nf_instance->client;
    ogs_assert(client);

    client->cb = client_cb;

    ogs_list_for_each(&nf_instance->nf_service_list, nf_service) {
        client = nf_service->client;
        if (client)
            client->cb = client_cb;
    }
}

void smf_sbi_discover_and_send(
        OpenAPI_nf_type_e nf_type, smf_sess_t *sess, void *data,
        ogs_sbi_request_t *(*build)(smf_sess_t *sess, void *data))
{
    ogs_sbi_session_t *session = NULL;

    ogs_assert(sess);
    session = sess->sbi.session;
    ogs_assert(nf_type);
    ogs_assert(build);

    sess->sbi.nf_state_registered = smf_nf_state_registered;
    sess->sbi.client_wait.duration =
        smf_timer_cfg(SMF_TIMER_SBI_CLIENT_WAIT)->duration;

    if (ogs_sbi_discover_and_send(
            nf_type, &sess->sbi, data, (ogs_sbi_build_f)build) != true) {
        ogs_sbi_server_send_error(session,
                OGS_SBI_HTTP_STATUS_GATEWAY_TIMEOUT, NULL,
                "Cannot discover", sess->supi);
    }
}

void smf_sbi_send_sm_context_create_error(
        ogs_sbi_session_t *session,
        int status, const char *title, const char *detail,
        ogs_pkbuf_t *n1smbuf)
{
    ogs_sbi_message_t sendmsg;
    ogs_sbi_response_t *response = NULL;

    OpenAPI_sm_context_create_error_t SmContextCreateError;
    OpenAPI_problem_details_t problem;
    OpenAPI_ref_to_binary_data_t n1_sm_msg;

    ogs_assert(session);

    memset(&problem, 0, sizeof(problem));
    problem.status = status;
    problem.title = (char*)title;
    problem.detail = (char*)detail;

    memset(&sendmsg, 0, sizeof(sendmsg));
    sendmsg.SmContextCreateError = &SmContextCreateError;

    memset(&SmContextCreateError, 0, sizeof(SmContextCreateError));
    SmContextCreateError.error = &problem;

    if (n1smbuf) {
        SmContextCreateError.n1_sm_msg = &n1_sm_msg;
        n1_sm_msg.content_id = (char *)OGS_SBI_CONTENT_5GNAS_SM_ID;
        sendmsg.part[0].content_id = (char *)OGS_SBI_CONTENT_5GNAS_SM_ID;
        sendmsg.part[0].content_type = (char *)OGS_SBI_CONTENT_5GNAS_TYPE;
        sendmsg.part[0].pkbuf = n1smbuf;
        sendmsg.num_of_part = 1;
    }

    response = ogs_sbi_build_response(&sendmsg, problem.status);
    ogs_assert(response);

    ogs_sbi_server_send_response(session, response);

    if (n1smbuf)
        ogs_pkbuf_free(n1smbuf);
}

void smf_sbi_send_sm_context_update_error(
        ogs_sbi_session_t *session,
        int status, const char *title, const char *detail,
        ogs_pkbuf_t *n2smbuf)
{
    ogs_sbi_message_t sendmsg;
    ogs_sbi_response_t *response = NULL;

    OpenAPI_sm_context_update_error_t SmContextUpdateError;
    OpenAPI_problem_details_t problem;
    OpenAPI_ref_to_binary_data_t n2_sm_info;

    ogs_assert(session);

    memset(&problem, 0, sizeof(problem));
    problem.status = status;
    problem.title = (char*)title;
    problem.detail = (char*)detail;

    memset(&sendmsg, 0, sizeof(sendmsg));
    sendmsg.SmContextUpdateError = &SmContextUpdateError;

    memset(&SmContextUpdateError, 0, sizeof(SmContextUpdateError));
    SmContextUpdateError.error = &problem;

    if (n2smbuf) {
        SmContextUpdateError.n2_sm_info = &n2_sm_info;
        n2_sm_info.content_id = (char *)OGS_SBI_CONTENT_NGAP_SM_ID;
        sendmsg.part[0].content_id = (char *)OGS_SBI_CONTENT_NGAP_SM_ID;
        sendmsg.part[0].content_type = (char *)OGS_SBI_CONTENT_NGAP_TYPE;
        sendmsg.part[0].pkbuf = n2smbuf;
        sendmsg.num_of_part = 1;
    }

    response = ogs_sbi_build_response(&sendmsg, problem.status);
    ogs_assert(response);

    ogs_sbi_server_send_response(session, response);

    if (n2smbuf)
        ogs_pkbuf_free(n2smbuf);
}