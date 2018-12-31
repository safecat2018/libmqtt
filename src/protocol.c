#include <stdio.h>

#include "debug.h"
#include <platform.h>

#include "mqtt_internal.h"
#include "packet.h"
#include "buffer.h"

typedef struct {
    PublishPayload *payload;
    MQTTPublishEventHandler callback;
    MQTTQosLevel qos;
} PublishCallback;

/*
 * Utility
 */

MQTTErrorCode send_buffer(MQTTHandle *handle, Buffer *buffer) {
    MQTTErrorCode ret = platform_write(handle, buffer);
    buffer_release(buffer);
    return ret;
}

/*
 * QoS event handlers
 */

void handle_puback_pubcomp(MQTTHandle *handle, void *context) {
    PublishCallback *ctx = (PublishCallback *)context;

    if (ctx->callback) {
        ctx->callback(handle, ctx->payload->topic, ctx->payload->message);
    }

    free(ctx->payload);
    free(ctx);
}

void handle_pubrec(MQTTHandle *handle, void *context) {
    PublishCallback *ctx = (PublishCallback *)context;

    PubRelPayload newPayload = {
        .packet_id = ctx->payload->packet_id
    };

    Buffer *encoded = mqtt_packet_encode(&(MQTTPacket){ PacketTypePubRel, &newPayload });
    expect_packet(handle, PacketTypePubComp, ctx->payload->packet_id, handle_puback_pubcomp, context);

    encoded->position = 0;
    send_buffer(handle, encoded);
}

void handle_pubrel(MQTTHandle *handle, void *context) {
    PublishCallback *ctx = (PublishCallback *)context;

    PubCompPayload newPayload = {
        .packet_id = ctx->payload->packet_id
    };

    Buffer *encoded = mqtt_packet_encode(&(MQTTPacket){ PacketTypePubComp, &newPayload });
    encoded->position = 0;
    if (send_buffer(handle, encoded) == MQTT_Error_Ok) {
        if (ctx->callback) {
            ctx->callback(handle, ctx->payload->topic, ctx->payload->message);
        }
    }

    free(ctx->payload->topic);
    free(ctx->payload->message);
    free(ctx->payload);
    free(ctx);
}

/*
 * packet constructors
 */

#if MQTT_CLIENT
MQTTErrorCode send_connect_packet(MQTTHandle *handle) {
    ConnectPayload *payload = (ConnectPayload *)calloc(1, sizeof(ConnectPayload));

    payload->client_id = handle->config->client_id;
    payload->protocol_level = 4;
    payload->keepalive_interval = KEEPALIVE_INTERVAL;
    payload->clean_session = handle->config->clean_session;

    payload->will_topic = handle->config->last_will_topic;
    payload->will_message = handle->config->last_will_message;
    payload->will_qos = MQTT_QOS_0;
    payload->retain_will = handle->config->last_will_retain;

    payload->username = handle->config->username;
    payload->password = handle->config->password;

    Buffer *encoded = mqtt_packet_encode(&(MQTTPacket){ PacketTypeConnect, payload });
    free(payload);

    // ConnAck waiting packet added to queue from _mqtt_connect

    encoded->position = 0;
    return send_buffer(handle, encoded);
}
#endif /* MQTT_CLIENT */

void remove_pending(MQTTHandle *handle, void *context) {
    SubscribePayload *payload = (SubscribePayload *)context;

    subscription_set_pending(handle, payload->topic, false);

    free(payload->topic);
    free(payload);
}

#if MQTT_CLIENT
MQTTErrorCode send_subscribe_packet(MQTTHandle *handle, char *topic, MQTTQosLevel qos) {
    SubscribePayload *payload = (SubscribePayload *)calloc(1, sizeof(SubscribePayload));

    payload->packet_id = (++handle->packet_id_counter > 0) ? handle->packet_id_counter : ++handle->packet_id_counter;
    payload->topic = strdup(topic);
    payload->qos = qos;

    Buffer *encoded = mqtt_packet_encode(&(MQTTPacket){ PacketTypeSubscribe, payload });

    // add waiting for SubAck to queue
    expect_packet(handle, PacketTypeSubAck, payload->packet_id, remove_pending, payload);

    encoded->position = 0;
    return send_buffer(handle, encoded);
}
#endif /* MQTT_CLIENT */

#if MQTT_CLIENT
MQTTErrorCode send_unsubscribe_packet(MQTTHandle *handle, char *topic) {
    UnsubscribePayload *payload = (UnsubscribePayload *)calloc(1, sizeof(UnsubscribePayload));

    payload->packet_id = (++handle->packet_id_counter > 0) ? handle->packet_id_counter : ++handle->packet_id_counter;
    payload->topic = topic;

    Buffer *encoded = mqtt_packet_encode(&(MQTTPacket){ PacketTypeUnsubscribe, payload });

    // add waiting for UnsubAck to queue
    expect_packet(handle, PacketTypeUnsubAck, payload->packet_id, NULL, NULL);
    free(payload);

    encoded->position = 0;
    return send_buffer(handle, encoded);
}
#endif /* MQTT_CLIENT */

MQTTErrorCode send_publish_packet(MQTTHandle *handle, char *topic, char *message, MQTTQosLevel qos, MQTTPublishEventHandler callback) {
    PublishPayload *payload = (PublishPayload *)calloc(1, sizeof(PublishPayload));

    payload->qos = qos;
    payload->retain = true;
    payload->topic = topic;
    payload->packet_id = (++handle->packet_id_counter > 0) ? handle->packet_id_counter : ++handle->packet_id_counter;
    payload->message = message;

    Buffer *encoded = mqtt_packet_encode(&(MQTTPacket){ PacketTypePublish, payload });
    encoded->position = 0;
    bool result = send_buffer(handle, encoded);
    if (result != MQTT_Error_Ok) {
        free(payload);
        return result;
    }

    // Handle QoS and add waiting packets to queue
    switch(payload->qos) {
        case MQTT_QOS_0:
            // fire and forget
            if (callback) {
                callback(handle, payload->topic, payload->message);
            }
            free(payload);
            break;
        case MQTT_QOS_1: {
            PublishCallback *ctx = (PublishCallback *)malloc(sizeof(PublishCallback));
            ctx->payload = payload;
            ctx->callback = callback;
            ctx->qos = payload->qos;
            expect_packet(handle, PacketTypePubAck, payload->packet_id, handle_puback_pubcomp, ctx);
            break;
        }
        case MQTT_QOS_2: {
            PublishCallback *ctx = (PublishCallback *)malloc(sizeof(PublishCallback));
            ctx->payload = payload;
            ctx->callback = callback;
            ctx->qos = payload->qos;
            expect_packet(handle, PacketTypePubRec, payload->packet_id, handle_pubrec, ctx);
            break;
        }
    }

    return true;
}

#if MQTT_CLIENT
MQTTErrorCode send_ping_packet(MQTTHandle *handle) {
    Buffer *encoded = mqtt_packet_encode(&(MQTTPacket){ PacketTypePingReq, NULL });
    encoded->position = 0;
    return send_buffer(handle, encoded);
}
#endif /* MQTT_CLIENT */

#if MQTT_CLIENT
MQTTErrorCode send_disconnect_packet(MQTTHandle *handle) {
    Buffer *encoded = mqtt_packet_encode(&(MQTTPacket){ PacketTypeDisconnect, NULL });
    encoded->position = 0;
    return send_buffer(handle, encoded);
}
#endif /* MQTT_CLIENT */

#if MQTT_CLIENT
MQTTErrorCode send_puback_packet(MQTTHandle *handle, uint16_t packet_id) {
    PacketIDPayload payload = { 0 };
    payload.packet_id = packet_id;

    DEBUG_LOG("Sending PUBACK");
    Buffer *encoded = mqtt_packet_encode(&(MQTTPacket){ PacketTypePubAck, &payload });
    encoded->position = 0;
    return send_buffer(handle, encoded);
}
#endif /* MQTT_CLIENT */

#if MQTT_CLIENT
MQTTErrorCode send_pubrec_packet(MQTTHandle *handle, uint16_t packet_id, MQTTPublishEventHandler callback, PublishPayload *publish) {
    PacketIDPayload payload = { 0 };
    payload.packet_id = packet_id;

    PublishCallback *ctx = (PublishCallback *)malloc(sizeof(PublishCallback));
    ctx->payload = malloc(sizeof(PublishPayload));
    memcpy(ctx->payload, publish, sizeof(PublishPayload));
    ctx->payload->topic = strdup(publish->topic);
    ctx->payload->message = strdup(publish->message);
    ctx->callback = callback;
    ctx->qos = MQTT_QOS_2;

    expect_packet(handle, PacketTypePubRel, packet_id, handle_pubrel, ctx);

    Buffer *encoded = mqtt_packet_encode(&(MQTTPacket){ PacketTypePubRec, &payload });
    encoded->position = 0;
    return send_buffer(handle, encoded);
}
#endif /* MQTT_CLIENT */
