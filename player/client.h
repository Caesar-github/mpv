#ifndef MP_CLIENT_H_
#define MP_CLIENT_H_

#include <stdint.h>

#include "libmpv/client.h"

struct MPContext;
struct mpv_handle;
struct mp_client_api;
struct mp_log;

void mp_clients_init(struct MPContext *mpctx);
void mp_clients_destroy(struct MPContext *mpctx);
int mp_clients_num(struct MPContext *mpctx);

void mp_client_broadcast_event(struct MPContext *mpctx, int event, void *data);
int mp_client_send_event(struct MPContext *mpctx, const char *client_name,
                         int event, void *data);
void mp_client_property_change(struct MPContext *mpctx, const char *name);

struct mpv_handle *mp_new_client(struct mp_client_api *clients, const char *name);
struct mp_log *mp_client_get_log(struct mpv_handle *ctx);
struct MPContext *mp_client_get_core(struct mpv_handle *ctx);

#endif
