/*
 * Minimal Secure Streams WebSocket Broadcast Example
 *
 * This example demonstrates pushing data to all connected Secure Streams WebSocket clients.
 * Compile with: gcc -o minimal-ss-ws-broadcast minimal-ss-ws-broadcast.c -lwebsockets
 *
 * License: MIT (same as libwebsockets minimal examples)
 */

#include <libwebsockets.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/* Global context and interrupt flag */
static struct lws_context *context;
static volatile int interrupted;

/* Mutex and linked list for tracking clients */
static pthread_mutex_t client_list_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Structure to hold client handles in a linked list */
struct client_list {
    struct lws_ss_handle *ssh;
    struct client_list *next;
};
static struct client_list *clients = NULL;

/* Utility: Count number of clients (for logging) */
static int client_count(void) {
    int count = 0;
    struct client_list *node = clients;
    while (node) {
        count++;
        node = node->next;
    }
    return count;
}

/* Add a client to the list */
static void add_client(struct lws_ss_handle *ssh) {
    struct client_list *new_client = malloc(sizeof(struct client_list));
    if (!new_client) {
        lwsl_err("%s: Failed to allocate client node\n", __func__);
        return;
    }
    new_client->ssh = ssh;
    pthread_mutex_lock(&client_list_mutex);
    new_client->next = clients;
    clients = new_client;
    pthread_mutex_unlock(&client_list_mutex);
    lwsl_notice("Client added, total clients: %d\n", client_count());
}

/* Remove a client from the list */
static void remove_client(struct lws_ss_handle *ssh) {
    struct client_list **p = &clients, *node;
    pthread_mutex_lock(&client_list_mutex);
    while (*p) {
        if ((*p)->ssh == ssh) {
            node = *p;
            *p = node->next;
            free(node);
            break;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&client_list_mutex);
    lwsl_notice("Client removed, total clients: %d\n", client_count());
}

/* Broadcast message to all connected clients */
static void broadcast_message(const char *msg, size_t len) {
    struct client_list *node;
    pthread_mutex_lock(&client_list_mutex);
    node = clients;
    while (node) {
        if (lws_ss_request_tx(node->ssh)) {
            lwsl_err("Failed to request tx for client\n");
        } else {
            if (lws_ss_server_tx(node->ssh, (uint8_t *)msg, len, LWS_WRITE_TEXT) < 0) {
                lwsl_err("Failed to send message to client\n");
            }
        }
        node = node->next;
    }
    pthread_mutex_unlock(&client_list_mutex);
}

/* Secure Streams state callback */
static int ss_state_callback(struct lws_ss_handle *ssh, void *user, lws_ss_constate_t state, void *data, size_t len) {
    switch (state) {
        case LWS_SS_STATE_CREATED:
            add_client(ssh);
            break;
        case LWS_SS_STATE_DISCONNECTED:
            remove_client(ssh);
            break;
        case LWS_SS_STATE_OPERATIONAL:
            lwsl_notice("Client operational\n");
            break;
        default:
            break;
    }
    return 0;
}

/* Thread to periodically broadcast messages */
static void *broadcast_thread(void *arg) {
    while (!interrupted) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Server time: %ld", (long)time(NULL));
        broadcast_message(msg, strlen(msg));
        sleep(5); /* Broadcast every 5 seconds */
    }
    return NULL;
}

/* Signal handler for graceful shutdown */
static void signal_handler(int sig) {
    interrupted = 1;
}

int main(int argc, char *argv[]) {
    struct lws_context_creation_info info = {0};
    pthread_t thread;

    /* Set up signal handling */
    signal(SIGINT, signal_handler);

    /* Context creation info */
    info.port = 443; /* Use WSS on port 443 */
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.protocols = (struct lws_protocols[]){
        {"ws", lws_callback_http_dummy, 0, 0},
        {NULL, NULL, 0, 0} /* Terminator */
    };
    info.ss_policy_name = "default"; /* Assumes a policy is defined */

    /* Create LWS context */
    context = lws_create_context(&info);
    if (!context) {
        lwsl_err("Failed to create LWS context\n");
        return 1;
    }

    /* Set up Secure Streams */
    struct lws_ss_info ssi = {0};
    ssi.handle_offset = 0;
    ssi.state = ss_state_callback;
    ssi.protocol = "ws";
    if (lws_ss_create(context, 0, &ssi, NULL, NULL, NULL, NULL)) {
        lwsl_err("Failed to create Secure Streams\n");
        lws_context_destroy(context);
        return 1;
    }

    /* Start the broadcast thread */
    if (pthread_create(&thread, NULL, broadcast_thread, NULL)) {
        lwsl_err("Failed to create broadcast thread\n");
        lws_context_destroy(context);
        return 1;
    }

    /* Main event loop */
    while (!interrupted) {
        lws_service(context, 0);
    }

    /* Cleanup */
    lws_context_destroy(context);
    pthread_join(thread, NULL);

    /* Free remaining client list (if any) */
    while (clients) {
        struct client_list *node = clients;
        clients = clients->next;
        free(node);
    }

    lwsl_notice("Server shut down\n");
    return 0;
}