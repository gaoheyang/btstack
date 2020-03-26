/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "mesh_upper_transport.c"

#include "mesh/mesh_upper_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack_util.h"
#include "btstack_memory.h"
#include "btstack_debug.h"
#include "btstack_bool.h"

#include "mesh/beacon.h"
#include "mesh/mesh_iv_index_seq_number.h"
#include "mesh/mesh_keys.h"
#include "mesh/mesh_lower_transport.h"
#include "mesh/mesh_peer.h"
#include "mesh/mesh_virtual_addresses.h"

// TODO: extract mesh_pdu functions into lower transport or network
#include "mesh/mesh_access.h"

// combined key x address iterator for upper transport decryption

typedef struct {
    // state
    mesh_transport_key_iterator_t  key_it;
    mesh_virtual_address_iterator_t address_it;
    // elements
    const mesh_transport_key_t *   key;
    const mesh_virtual_address_t * address;
    // address - might be virtual
    uint16_t dst;
    // key info
} mesh_transport_key_and_virtual_address_iterator_t;

static void mesh_upper_transport_validate_segmented_message(void);
static void mesh_upper_transport_run(void);

// upper transport callbacks - in access layer
static void (*mesh_access_message_handler)( mesh_transport_callback_type_t callback_type, mesh_transport_status_t status, mesh_pdu_t * pdu);
static void (*mesh_control_message_handler)( mesh_transport_callback_type_t callback_type, mesh_transport_status_t status, mesh_pdu_t * pdu);

//
static int crypto_active;
static uint8_t application_nonce[13];
static btstack_crypto_ccm_t ccm;

static mesh_transport_key_and_virtual_address_iterator_t mesh_transport_key_it;

static mesh_access_pdu_t *   incoming_access_pdu_encrypted;
static mesh_access_pdu_t *   incoming_access_pdu_decrypted;

static mesh_control_pdu_t *  incoming_control_pdu;

static mesh_access_pdu_t     incoming_access_pdu_encrypted_singleton;

static mesh_pdu_t *          incoming_access_encrypted;

static union {
    mesh_control_pdu_t    control;
    mesh_access_pdu_t     access;
} incoming_pdu_singleton;

// incoming unsegmented (network) and segmented (transport) control and access messages
static btstack_linked_list_t upper_transport_incoming;

// outgoing unsegmented and segmented control and access messages
static btstack_linked_list_t upper_transport_outgoing;

// outgoing upper transport messages that have been sent to lower transport and wait for sent event
static btstack_linked_list_t upper_transport_outgoing_active;

// TODO: higher layer define used for assert
#define MESH_ACCESS_OPCODE_NOT_SET 0xFFFFFFFEu

static void mesh_print_hex(const char * name, const uint8_t * data, uint16_t len){
    printf("%-20s ", name);
    printf_hexdump(data, len);
}
// static void mesh_print_x(const char * name, uint32_t value){
//     printf("%20s: 0x%x", name, (int) value);
// }

static void mesh_transport_key_and_virtual_address_iterator_init(mesh_transport_key_and_virtual_address_iterator_t *it,
                                                                 uint16_t dst, uint16_t netkey_index, uint8_t akf,
                                                                 uint8_t aid) {
    printf("KEY_INIT: dst %04x, akf %x, aid %x\n", dst, akf, aid);
    // config
    it->dst   = dst;
    // init elements
    it->key     = NULL;
    it->address = NULL;
    // init element iterators
    mesh_transport_key_aid_iterator_init(&it->key_it, netkey_index, akf, aid);
    // init address iterator
    if (mesh_network_address_virtual(it->dst)){
        mesh_virtual_address_iterator_init(&it->address_it, dst);
        // get first key
        if (mesh_transport_key_aid_iterator_has_more(&it->key_it)) {
            it->key = mesh_transport_key_aid_iterator_get_next(&it->key_it);
        }
    }
}

// cartesian product: keys x addressses
static int mesh_transport_key_and_virtual_address_iterator_has_more(mesh_transport_key_and_virtual_address_iterator_t * it){
    if (mesh_network_address_virtual(it->dst)) {
        // find next valid entry
        while (true){
            if (mesh_virtual_address_iterator_has_more(&it->address_it)) return 1;
            if (!mesh_transport_key_aid_iterator_has_more(&it->key_it)) return 0;
            // get next key
            it->key = mesh_transport_key_aid_iterator_get_next(&it->key_it);
            mesh_virtual_address_iterator_init(&it->address_it, it->dst);
        }
    } else {
        return mesh_transport_key_aid_iterator_has_more(&it->key_it);
    }
}

static void mesh_transport_key_and_virtual_address_iterator_next(mesh_transport_key_and_virtual_address_iterator_t * it){
    if (mesh_network_address_virtual(it->dst)) {
        it->address = mesh_virtual_address_iterator_get_next(&it->address_it);
    } else {
        it->key = mesh_transport_key_aid_iterator_get_next(&it->key_it);
    }
}

// UPPER TRANSPORT

uint16_t mesh_access_dst(mesh_access_pdu_t * access_pdu){
    return big_endian_read_16(access_pdu->network_header, 7);
}

uint16_t mesh_access_ctl(mesh_access_pdu_t * access_pdu){
    return access_pdu->network_header[1] >> 7;
}

uint32_t mesh_access_seq(mesh_access_pdu_t * access_pdu){
    return big_endian_read_24(access_pdu->network_header, 2);
}

void mesh_access_set_nid_ivi(mesh_access_pdu_t * access_pdu, uint8_t nid_ivi){
    access_pdu->network_header[0] = nid_ivi;
}
void mesh_access_set_ctl_ttl(mesh_access_pdu_t * access_pdu, uint8_t ctl_ttl){
    access_pdu->network_header[1] = ctl_ttl;
}
void mesh_access_set_seq(mesh_access_pdu_t * access_pdu, uint32_t seq){
    big_endian_store_24(access_pdu->network_header, 2, seq);
}
void mesh_access_set_src(mesh_access_pdu_t * access_pdu, uint16_t src){
    big_endian_store_16(access_pdu->network_header, 5, src);
}
void mesh_access_set_dest(mesh_access_pdu_t * access_pdu, uint16_t dest){
    big_endian_store_16(access_pdu->network_header, 7, dest);
}

static void mesh_segmented_pdu_flatten(btstack_linked_list_t * segments, uint8_t segment_len, uint8_t * buffer) {
    // assemble payload
    btstack_linked_list_iterator_t it;
    btstack_linked_list_iterator_init(&it, segments);
    while (btstack_linked_list_iterator_has_next(&it)) {
        mesh_network_pdu_t *segment = (mesh_network_pdu_t *) btstack_linked_list_iterator_next(&it);
        btstack_assert(segment->pdu_header.pdu_type == MESH_PDU_TYPE_NETWORK);
        // get segment n
        uint8_t *lower_transport_pdu = mesh_network_pdu_data(segment);
        uint8_t seg_o = (big_endian_read_16(lower_transport_pdu, 2) >> 5) & 0x001f;
        uint8_t *segment_data = &lower_transport_pdu[4];
        (void) memcpy(&buffer[seg_o * segment_len], segment_data, segment_len);
    }
}

static uint16_t mesh_upper_pdu_flatten(mesh_upper_transport_pdu_t * upper_pdu, uint8_t * buffer, uint16_t buffer_len) {
    // assemble payload
    btstack_linked_list_iterator_t it;
    btstack_linked_list_iterator_init(&it, &upper_pdu->segments);
    uint16_t offset = 0;
    while (btstack_linked_list_iterator_has_next(&it)) {
        mesh_network_pdu_t *segment = (mesh_network_pdu_t *) btstack_linked_list_iterator_next(&it);
        btstack_assert(segment->pdu_header.pdu_type == MESH_PDU_TYPE_NETWORK);
        btstack_assert((offset + segment->len) <= buffer_len);
        (void) memcpy(&buffer[offset], segment->data, segment->len);
        offset += segment->len;
    }
    return offset;
}

// store payload in provided list of network pdus
static void mesh_segmented_store_payload(const uint8_t * payload, uint16_t payload_len, btstack_linked_list_t * in_segments, btstack_linked_list_t * out_segments){
    uint16_t payload_offset = 0;
    uint16_t bytes_current_segment = 0;
    mesh_network_pdu_t * network_pdu = NULL;
    while (payload_offset < payload_len){
        if (bytes_current_segment == 0){
            network_pdu = (mesh_network_pdu_t *) btstack_linked_list_pop(in_segments);
            btstack_assert(network_pdu != NULL);
            btstack_linked_list_add_tail(out_segments, (btstack_linked_item_t *) network_pdu);
            bytes_current_segment = MESH_NETWORK_PAYLOAD_MAX;
        }
        uint16_t bytes_to_copy = btstack_min(bytes_current_segment, payload_len - payload_offset);
        (void) memcpy(&network_pdu->data[network_pdu->len], &payload[payload_offset], bytes_to_copy);
        bytes_current_segment -= bytes_to_copy;
        network_pdu->len += bytes_to_copy;
        payload_offset += bytes_to_copy;
    }
}

// tries allocate and add enough segments to store payload of given size
static bool mesh_segmented_allocate_segments(btstack_linked_list_t * segments, uint16_t payload_len){
    uint16_t storage_size = btstack_linked_list_count(segments) * MESH_NETWORK_PAYLOAD_MAX;
    while (storage_size < payload_len){
        mesh_network_pdu_t * network_pdu = mesh_network_pdu_get();
        if (network_pdu == NULL) break;
        storage_size += MESH_NETWORK_PAYLOAD_MAX;
        btstack_linked_list_add(segments, (btstack_linked_item_t *) network_pdu);
    }
    return (storage_size >= payload_len);
}

// stub lower transport

static void mesh_upper_transport_dump_pdus(const char *name, btstack_linked_list_t *list){
    printf("List: %s:\n", name);
    btstack_linked_list_iterator_t it;
    btstack_linked_list_iterator_init(&it, list);
    while (btstack_linked_list_iterator_has_next(&it)){
        mesh_pdu_t * pdu = (mesh_pdu_t*) btstack_linked_list_iterator_next(&it);
        printf("- %p\n", pdu);
        // printf_hexdump( mesh_pdu_data(pdu), mesh_pdu_len(pdu));
    }
}

static void mesh_upper_transport_reset_pdus(btstack_linked_list_t *list){
    while (!btstack_linked_list_empty(list)){
        mesh_upper_transport_pdu_free((mesh_pdu_t *) btstack_linked_list_pop(list));
    }
}

void mesh_upper_transport_dump(void){
    mesh_upper_transport_dump_pdus("upper_transport_incoming", &upper_transport_incoming);
}

void mesh_upper_transport_reset(void){
    crypto_active = 0;
    mesh_upper_transport_reset_pdus(&upper_transport_incoming);
}

static mesh_transport_key_t * mesh_upper_transport_get_outgoing_appkey(uint16_t netkey_index, uint16_t appkey_index){
    // Device Key is fixed
    if (appkey_index == MESH_DEVICE_KEY_INDEX) {
        return mesh_transport_key_get(appkey_index);
    }

    // Get key refresh state from subnet
    mesh_subnet_t * subnet = mesh_subnet_get_by_netkey_index(netkey_index);
    if (subnet == NULL) return NULL;

    // identify old and new app keys for given appkey_index
    mesh_transport_key_t * old_key = NULL;
    mesh_transport_key_t * new_key = NULL;
    mesh_transport_key_iterator_t it;
    mesh_transport_key_iterator_init(&it, netkey_index);
    while (mesh_transport_key_iterator_has_more(&it)){
        mesh_transport_key_t * transport_key = mesh_transport_key_iterator_get_next(&it);
        if (transport_key->appkey_index != appkey_index) continue;
        if (transport_key->old_key == 0) {
            new_key = transport_key;
        } else {
            old_key = transport_key;
        }
    }

    // if no key is marked as old, just use the current one
    if (old_key == NULL) return new_key;

    // use new key if it exists in phase two
    if ((subnet->key_refresh == MESH_KEY_REFRESH_SECOND_PHASE) && (new_key != NULL)){
        return new_key;
    } else {
        return old_key;
    }
}

static uint32_t iv_index_for_ivi_nid(uint8_t ivi_nid){
    // get IV Index and IVI
    uint32_t iv_index = mesh_get_iv_index();
    int ivi = ivi_nid >> 7;

    // if least significant bit differs, use previous IV Index
    if ((iv_index & 1 ) ^ ivi){
        iv_index--;
    }
    return iv_index;
}

static void transport_segmented_setup_nonce(uint8_t * nonce, const mesh_pdu_t * pdu){
    mesh_access_pdu_t * access_pdu;
    mesh_upper_transport_pdu_t * upper_pdu;
    switch (pdu->pdu_type){
        case MESH_PDU_TYPE_ACCESS:
            access_pdu = (mesh_access_pdu_t *) pdu;
            nonce[1] = access_pdu->transmic_len == 8 ? 0x80 : 0x00;
            (void)memcpy(&nonce[2], &access_pdu->network_header[2], 7);
            big_endian_store_32(nonce, 9, iv_index_for_ivi_nid(access_pdu->network_header[0]));
            break;
        case MESH_PDU_TYPE_UPPER_SEGMENTED_ACCESS:
        case MESH_PDU_TYPE_UPPER_UNSEGMENTED_ACCESS:
            upper_pdu = (mesh_upper_transport_pdu_t *) pdu;
            nonce[1] = upper_pdu->transmic_len == 8 ? 0x80 : 0x00;
            // 'network header'
            big_endian_store_24(nonce, 2, upper_pdu->seq);
            big_endian_store_16(nonce, 5, upper_pdu->src);
            big_endian_store_16(nonce, 7, upper_pdu->dst);
            big_endian_store_32(nonce, 9, iv_index_for_ivi_nid(upper_pdu->ivi_nid));
            break;
        default:
            btstack_assert(0);
            break;
    }
}

static void transport_segmented_setup_application_nonce(uint8_t * nonce, const mesh_pdu_t * pdu){
    nonce[0] = 0x01;
    transport_segmented_setup_nonce(nonce, pdu);
    mesh_print_hex("AppNonce", nonce, 13);
}

static void transport_segmented_setup_device_nonce(uint8_t * nonce, const mesh_pdu_t * pdu){
    nonce[0] = 0x02;
    transport_segmented_setup_nonce(nonce, pdu);
    mesh_print_hex("DeviceNonce", nonce, 13);
}

static void mesh_upper_transport_process_access_message_done(mesh_access_pdu_t *access_pdu){
    crypto_active = 0;
    btstack_assert(mesh_access_ctl(access_pdu) == 0);
    mesh_lower_transport_message_processed_by_higher_layer(incoming_access_encrypted);
    incoming_access_encrypted = NULL;
    incoming_access_pdu_encrypted = NULL;
    mesh_upper_transport_run();
}

static void mesh_upper_transport_process_control_message_done(mesh_control_pdu_t * control_pdu){
    crypto_active = 0;
    incoming_control_pdu = NULL;
    mesh_upper_transport_run();
}

static void mesh_upper_transport_validate_segmented_message_ccm(void * arg){
    UNUSED(arg);

    uint8_t * upper_transport_pdu     = incoming_access_pdu_decrypted->data;
    uint8_t   upper_transport_pdu_len = incoming_access_pdu_decrypted->len - incoming_access_pdu_decrypted->transmic_len;
 
    mesh_print_hex("Decrypted PDU", upper_transport_pdu, upper_transport_pdu_len);

    // store TransMIC
    uint8_t trans_mic[8];
    btstack_crypto_ccm_get_authentication_value(&ccm, trans_mic);
    mesh_print_hex("TransMIC", trans_mic, incoming_access_pdu_decrypted->transmic_len);

    if (memcmp(trans_mic, &upper_transport_pdu[upper_transport_pdu_len], incoming_access_pdu_decrypted->transmic_len) == 0){
        printf("TransMIC matches\n");

        // remove TransMIC from payload
        incoming_access_pdu_decrypted->len -= incoming_access_pdu_decrypted->transmic_len;

        // if virtual address, update dst to pseudo_dst
        if (mesh_network_address_virtual(mesh_access_dst(incoming_access_pdu_decrypted))){
            big_endian_store_16(incoming_access_pdu_decrypted->network_header, 7, mesh_transport_key_it.address->pseudo_dst);
        }

        // pass to upper layer
        btstack_assert(mesh_access_message_handler != NULL);
        mesh_pdu_t * pdu = (mesh_pdu_t*) incoming_access_pdu_decrypted;
        mesh_access_message_handler(MESH_TRANSPORT_PDU_RECEIVED, MESH_TRANSPORT_STATUS_SUCCESS, pdu);

        printf("\n");

    } else {
        uint8_t akf = incoming_access_pdu_decrypted->akf_aid_control & 0x40;
        if (akf){
            printf("TransMIC does not match, try next key\n");
            mesh_upper_transport_validate_segmented_message();
        } else {
            printf("TransMIC does not match device key, done\n");
            // done
            mesh_upper_transport_process_access_message_done(incoming_access_pdu_decrypted);
        }
    }
}

static void mesh_upper_transport_validate_segmented_message_digest(void * arg){
    UNUSED(arg);
    uint8_t   upper_transport_pdu_len      = incoming_access_pdu_encrypted->len - incoming_access_pdu_encrypted->transmic_len;
    uint8_t * upper_transport_pdu_data_in  = incoming_access_pdu_encrypted->data;
    uint8_t * upper_transport_pdu_data_out = incoming_access_pdu_decrypted->data;
    btstack_crypto_ccm_decrypt_block(&ccm, upper_transport_pdu_len, upper_transport_pdu_data_in, upper_transport_pdu_data_out, &mesh_upper_transport_validate_segmented_message_ccm, NULL);
}

static void mesh_upper_transport_validate_segmented_message(void){
    uint8_t * upper_transport_pdu_data =  incoming_access_pdu_decrypted->data;
    uint8_t   upper_transport_pdu_len  = incoming_access_pdu_decrypted->len - incoming_access_pdu_decrypted->transmic_len;

    if (!mesh_transport_key_and_virtual_address_iterator_has_more(&mesh_transport_key_it)){
        printf("No valid transport key found\n");
        mesh_upper_transport_process_access_message_done(incoming_access_pdu_decrypted);
        return;
    }
    mesh_transport_key_and_virtual_address_iterator_next(&mesh_transport_key_it);
    const mesh_transport_key_t * message_key = mesh_transport_key_it.key;

    if (message_key->akf){
        transport_segmented_setup_application_nonce(application_nonce, (mesh_pdu_t *) incoming_access_pdu_encrypted);
    } else {
        transport_segmented_setup_device_nonce(application_nonce, (mesh_pdu_t *) incoming_access_pdu_encrypted);
    }

    // store application / device key index
    mesh_print_hex("AppOrDevKey", message_key->key, 16);
    incoming_access_pdu_decrypted->appkey_index = message_key->appkey_index;

    mesh_print_hex("EncAccessPayload", upper_transport_pdu_data, upper_transport_pdu_len);

    // decrypt ccm
    crypto_active = 1;
    uint16_t aad_len  = 0;
    if (mesh_network_address_virtual(mesh_access_dst(incoming_access_pdu_decrypted))){
        aad_len  = 16;
    }
    btstack_crypto_ccm_init(&ccm, message_key->key, application_nonce, upper_transport_pdu_len, aad_len, incoming_access_pdu_decrypted->transmic_len);

    if (aad_len){
        btstack_crypto_ccm_digest(&ccm, (uint8_t *) mesh_transport_key_it.address->label_uuid, aad_len, &mesh_upper_transport_validate_segmented_message_digest, NULL);
    } else {
        mesh_upper_transport_validate_segmented_message_digest(NULL);
    }
}

static void mesh_upper_transport_process_segmented_message(void){
    // copy original pdu
    (void)memcpy(incoming_access_pdu_decrypted, incoming_access_pdu_encrypted,
                 sizeof(mesh_access_pdu_t));

    // 
    uint8_t * upper_transport_pdu     =  incoming_access_pdu_decrypted->data;
    uint8_t   upper_transport_pdu_len = incoming_access_pdu_decrypted->len - incoming_access_pdu_decrypted->transmic_len;
    mesh_print_hex("Upper Transport pdu", upper_transport_pdu, upper_transport_pdu_len);

    uint8_t aid = incoming_access_pdu_decrypted->akf_aid_control & 0x3f;
    uint8_t akf = (incoming_access_pdu_decrypted->akf_aid_control & 0x40) >> 6;

    printf("AKF: %u\n",   akf);
    printf("AID: %02x\n", aid);

    mesh_transport_key_and_virtual_address_iterator_init(&mesh_transport_key_it, mesh_access_dst(incoming_access_pdu_decrypted),
                                                         incoming_access_pdu_decrypted->netkey_index, akf, aid);
    mesh_upper_transport_validate_segmented_message();
}

static void mesh_upper_transport_message_received(mesh_pdu_t * pdu){
    btstack_linked_list_add_tail(&upper_transport_incoming, (btstack_linked_item_t*) pdu);
    mesh_upper_transport_run();
}

static void mesh_upper_transport_send_access_segmented(mesh_upper_transport_pdu_t * upper_pdu){

    mesh_segmented_pdu_t * segmented_pdu   = (mesh_segmented_pdu_t *) upper_pdu->lower_pdu;
    segmented_pdu->pdu_header.pdu_type = MESH_PDU_TYPE_SEGMENTED;

    // convert mesh_access_pdu_t into mesh_segmented_pdu_t
    btstack_linked_list_t free_segments = segmented_pdu->segments;
    segmented_pdu->segments = NULL;
    mesh_segmented_store_payload(incoming_pdu_singleton.access.data, upper_pdu->len, &free_segments, &segmented_pdu->segments);

    // copy meta
    segmented_pdu->len = upper_pdu->len;
    segmented_pdu->netkey_index = upper_pdu->netkey_index;
    segmented_pdu->transmic_len = upper_pdu->transmic_len;
    segmented_pdu->akf_aid_control = upper_pdu->akf_aid_control;
    segmented_pdu->flags = upper_pdu->flags;

    // setup segmented_pdu header
    // (void)memcpy(segmented_pdu->network_header, upper_pdu->network_header, 9);
    // TODO: use fields in mesh_segmented_pdu_t and setup network header in lower transport
    segmented_pdu->network_header[0] = upper_pdu->ivi_nid;
    segmented_pdu->network_header[1] = upper_pdu->ctl_ttl;
    big_endian_store_24(segmented_pdu->network_header, 2, upper_pdu->seq);
    big_endian_store_16(segmented_pdu->network_header, 5, upper_pdu->src);
    big_endian_store_16(segmented_pdu->network_header, 7, upper_pdu->dst);

    // queue up
    upper_pdu->lower_pdu = (mesh_pdu_t *) segmented_pdu;
    btstack_linked_list_add(&upper_transport_outgoing_active, (btstack_linked_item_t *) upper_pdu);

    mesh_lower_transport_send_pdu((mesh_pdu_t*) segmented_pdu);
}

static void mesh_upper_transport_send_access_unsegmented(mesh_upper_transport_pdu_t * upper_pdu){

    // provide segment
    mesh_network_pdu_t * network_pdu = (mesh_network_pdu_t *) upper_pdu->lower_pdu;

    // setup network pdu
    network_pdu->pdu_header.pdu_type = MESH_PDU_TYPE_UPPER_UNSEGMENTED_ACCESS;
    network_pdu->data[0] = upper_pdu->ivi_nid;
    network_pdu->data[1] = upper_pdu->ctl_ttl;
    big_endian_store_24(network_pdu->data, 2, upper_pdu->seq);
    big_endian_store_16(network_pdu->data, 5, upper_pdu->src);
    big_endian_store_16(network_pdu->data, 7, upper_pdu->dst);
    network_pdu->netkey_index = upper_pdu->netkey_index;

    // setup access message
    network_pdu->data[9] = upper_pdu->akf_aid_control;
    btstack_assert(upper_pdu->len < 15);
    (void)memcpy(&network_pdu->data[10], &incoming_pdu_singleton.access.data, upper_pdu->len);
    network_pdu->len = 10 + upper_pdu->len;
    network_pdu->flags = 0;

    // queue up
    btstack_linked_list_add(&upper_transport_outgoing_active, (btstack_linked_item_t *) upper_pdu);

    mesh_lower_transport_send_pdu((mesh_pdu_t*) network_pdu);
}

static void mesh_upper_transport_send_access_ccm(void * arg){
    crypto_active = 0;

    mesh_upper_transport_pdu_t * upper_pdu = (mesh_upper_transport_pdu_t *) arg;
    mesh_print_hex("EncAccessPayload", incoming_pdu_singleton.access.data, upper_pdu->len);
    // store TransMIC
    btstack_crypto_ccm_get_authentication_value(&ccm, &incoming_pdu_singleton.access.data[upper_pdu->len]);
    mesh_print_hex("TransMIC", &incoming_pdu_singleton.access.data[upper_pdu->len], upper_pdu->transmic_len);
    upper_pdu->len += upper_pdu->transmic_len;
    mesh_print_hex("UpperTransportPDU", incoming_pdu_singleton.access.data, upper_pdu->len);
    switch (upper_pdu->pdu_header.pdu_type){
        case MESH_PDU_TYPE_UPPER_UNSEGMENTED_ACCESS:
            mesh_upper_transport_send_access_unsegmented(upper_pdu);
            break;
        case MESH_PDU_TYPE_UPPER_SEGMENTED_ACCESS:
            mesh_upper_transport_send_access_segmented(upper_pdu);
            break;
        default:
            btstack_assert(false);
    }
}

static void mesh_upper_transport_send_access_digest(void *arg){
    mesh_upper_transport_pdu_t * upper_pdu = (mesh_upper_transport_pdu_t *) arg;
    uint16_t  access_pdu_len  = upper_pdu->len;
    btstack_crypto_ccm_encrypt_block(&ccm, access_pdu_len, incoming_pdu_singleton.access.data, incoming_pdu_singleton.access.data,
                                     &mesh_upper_transport_send_access_ccm, upper_pdu);
}

static void mesh_upper_transport_send_access(mesh_upper_transport_pdu_t * upper_pdu){

    // if dst is virtual address, lookup label uuid and hash
    uint16_t aad_len = 0;
    mesh_virtual_address_t * virtual_address = NULL;
    if (mesh_network_address_virtual(upper_pdu->dst)){
        virtual_address = mesh_virtual_address_for_pseudo_dst(upper_pdu->dst);
        if (!virtual_address){
            printf("No virtual address register for pseudo dst %4x\n", upper_pdu->dst);
            mesh_access_message_handler(MESH_TRANSPORT_PDU_SENT, MESH_TRANSPORT_STATUS_SEND_FAILED, (mesh_pdu_t *) upper_pdu);
            return;
        }
        // printf("Using hash %4x with LabelUUID: ", virtual_address->hash);
        // printf_hexdump(virtual_address->label_uuid, 16);
        aad_len = 16;
        upper_pdu->dst = virtual_address->hash;
    }

    // get app or device key
    uint16_t appkey_index = upper_pdu->appkey_index;
    const mesh_transport_key_t * appkey = mesh_upper_transport_get_outgoing_appkey(upper_pdu->netkey_index, appkey_index);
    if (appkey == NULL){
        printf("AppKey %04x not found, drop message\n", appkey_index);
        mesh_access_message_handler(MESH_TRANSPORT_PDU_SENT, MESH_TRANSPORT_STATUS_SEND_FAILED, (mesh_pdu_t *) upper_pdu);
        return;
    }

    // reserve slot
    mesh_lower_transport_reserve_slot();

    // reserve one sequence number, which is also used to encrypt access payload
    uint32_t seq = mesh_sequence_number_next();
    upper_pdu->flags |= MESH_TRANSPORT_FLAG_SEQ_RESERVED;
    upper_pdu->seq = seq;

    // also reserves crypto_buffer
    crypto_active = 1;

    // flatten segmented pdu into crypto buffer
    uint16_t payload_len = mesh_upper_pdu_flatten(upper_pdu, incoming_pdu_singleton.access.data, sizeof(incoming_pdu_singleton.access.data));
    btstack_assert(payload_len == upper_pdu->len);

    // Dump PDU
    printf("[+] Upper transport, send upper (un)segmented Access PDU - dest %04x, seq %06x\n", upper_pdu->dst, upper_pdu->seq);
    mesh_print_hex("Access Payload", incoming_pdu_singleton.access.data, upper_pdu->len);

    // setup nonce - uses dst, so after pseudo address translation
    if (appkey_index == MESH_DEVICE_KEY_INDEX){
        transport_segmented_setup_device_nonce(application_nonce, (mesh_pdu_t *) upper_pdu);
    } else {
        transport_segmented_setup_application_nonce(application_nonce, (mesh_pdu_t *) upper_pdu);
    }

    // Dump key
    mesh_print_hex("AppOrDevKey", appkey->key, 16);

    // encrypt ccm
    uint8_t   transmic_len    = upper_pdu->transmic_len;
    uint16_t  access_pdu_len  = upper_pdu->len;
    btstack_crypto_ccm_init(&ccm, appkey->key, application_nonce, access_pdu_len, aad_len, transmic_len);
    if (virtual_address){
        mesh_print_hex("LabelUUID", virtual_address->label_uuid, 16);
        btstack_crypto_ccm_digest(&ccm, virtual_address->label_uuid, 16,
                                  &mesh_upper_transport_send_access_digest, upper_pdu);
    } else {
        mesh_upper_transport_send_access_digest(upper_pdu);
    }
}

static void mesh_upper_transport_send_unsegmented_control_pdu(mesh_network_pdu_t * network_pdu){
    // reserve slot
    mesh_lower_transport_reserve_slot();
    // reserve sequence number
    uint32_t seq = mesh_sequence_number_next();
    mesh_network_pdu_set_seq(network_pdu, seq);
    // Dump PDU
    uint8_t opcode = network_pdu->data[9];
    printf("[+] Upper transport, send unsegmented Control PDU %p - seq %06x opcode %02x\n", network_pdu, seq, opcode);
    mesh_print_hex("Access Payload", &network_pdu->data[10], network_pdu->len - 10);

    // send
     mesh_lower_transport_send_pdu((mesh_pdu_t *) network_pdu);
}

static void mesh_upper_transport_send_segmented_control_pdu(mesh_upper_transport_pdu_t * upper_pdu){
    // reserve slot
    mesh_lower_transport_reserve_slot();
    // reserve sequence number
    uint32_t seq = mesh_sequence_number_next();
    upper_pdu->flags |= MESH_TRANSPORT_FLAG_SEQ_RESERVED;
    upper_pdu->seq = seq;
    // Dump PDU
    // uint8_t opcode = upper_pdu->data[0];
    // printf("[+] Upper transport, send segmented Control PDU %p - seq %06x opcode %02x\n", upper_pdu, seq, opcode);
    // mesh_print_hex("Access Payload", &upper_pdu->data[1], upper_pdu->len - 1);
    // send
    mesh_segmented_pdu_t * segmented_pdu   = (mesh_segmented_pdu_t *) upper_pdu->lower_pdu;
    segmented_pdu->pdu_header.pdu_type = MESH_PDU_TYPE_SEGMENTED;

    // lend segments to lower transport pdu
    segmented_pdu->segments = upper_pdu->segments;
    upper_pdu->segments = NULL;

    // copy meta
    segmented_pdu->len = upper_pdu->len;
    segmented_pdu->netkey_index = upper_pdu->netkey_index;
    segmented_pdu->transmic_len = 0;   // no TransMIC for control
    segmented_pdu->akf_aid_control = upper_pdu->akf_aid_control;
    segmented_pdu->flags = upper_pdu->flags;

    // setup segmented_pdu header
    // TODO: use fields in mesh_segmented_pdu_t and setup network header in lower transport
    segmented_pdu->network_header[0] = upper_pdu->ivi_nid;
    segmented_pdu->network_header[1] = upper_pdu->ctl_ttl;
    big_endian_store_24(segmented_pdu->network_header, 2, upper_pdu->seq);
    big_endian_store_16(segmented_pdu->network_header, 5, upper_pdu->src);
    big_endian_store_16(segmented_pdu->network_header, 7, upper_pdu->dst);

    // queue up
    upper_pdu->lower_pdu = (mesh_pdu_t *) segmented_pdu;
    btstack_linked_list_add(&upper_transport_outgoing_active, (btstack_linked_item_t *) upper_pdu);

    mesh_lower_transport_send_pdu((mesh_pdu_t *) segmented_pdu);
}

static void mesh_upper_transport_run(void){

    while(!btstack_linked_list_empty(&upper_transport_incoming)){

        if (crypto_active) return;

        // get next message
        mesh_pdu_t * pdu =  (mesh_pdu_t *) btstack_linked_list_pop(&upper_transport_incoming);
        mesh_network_pdu_t   * network_pdu;
        mesh_segmented_pdu_t   * message_pdu;
        switch (pdu->pdu_type){
            case MESH_PDU_TYPE_UNSEGMENTED:
                network_pdu = (mesh_network_pdu_t *) pdu;
                // control?
                if (mesh_network_control(network_pdu)) {

                    incoming_control_pdu =  &incoming_pdu_singleton.control;
                    incoming_control_pdu->pdu_header.pdu_type = MESH_PDU_TYPE_CONTROL;
                    incoming_control_pdu->len =  network_pdu->len;
                    incoming_control_pdu->netkey_index =  network_pdu->netkey_index;

                    uint8_t * lower_transport_pdu = mesh_network_pdu_data(network_pdu);

                    incoming_control_pdu->akf_aid_control = lower_transport_pdu[0];
                    incoming_control_pdu->len = network_pdu->len - 10; // 9 header + 1 opcode
                    (void)memcpy(incoming_control_pdu->data, &lower_transport_pdu[1], incoming_control_pdu->len);

                    // copy meta data into encrypted pdu buffer
                    (void)memcpy(incoming_control_pdu->network_header, network_pdu->data, 9);

                    mesh_print_hex("Assembled payload", incoming_control_pdu->data, incoming_control_pdu->len);

                    // free mesh message
                    mesh_lower_transport_message_processed_by_higher_layer(pdu);

                    btstack_assert(mesh_control_message_handler != NULL);
                    mesh_pdu_t * pdu = (mesh_pdu_t*) incoming_control_pdu;
                    mesh_control_message_handler(MESH_TRANSPORT_PDU_RECEIVED, MESH_TRANSPORT_STATUS_SUCCESS, pdu);

                } else {

                    incoming_access_encrypted = (mesh_pdu_t *) network_pdu;

                    incoming_access_pdu_encrypted = &incoming_access_pdu_encrypted_singleton;
                    incoming_access_pdu_encrypted->pdu_header.pdu_type = MESH_PDU_TYPE_ACCESS;
                    incoming_access_pdu_decrypted = &incoming_pdu_singleton.access;

                    incoming_access_pdu_encrypted->netkey_index = network_pdu->netkey_index;
                    incoming_access_pdu_encrypted->transmic_len = 4;

                    uint8_t * lower_transport_pdu = mesh_network_pdu_data(network_pdu);

                    incoming_access_pdu_encrypted->akf_aid_control = lower_transport_pdu[0];
                    incoming_access_pdu_encrypted->len = network_pdu->len - 10; // 9 header + 1 AID
                    (void)memcpy(incoming_access_pdu_encrypted->data, &lower_transport_pdu[1], incoming_access_pdu_encrypted->len);

                    // copy meta data into encrypted pdu buffer
                    (void)memcpy(incoming_access_pdu_encrypted->network_header, network_pdu->data, 9);

                    mesh_print_hex("Assembled payload", incoming_access_pdu_encrypted->data, incoming_access_pdu_encrypted->len);

                    // get encoded transport pdu and start processing
                    mesh_upper_transport_process_segmented_message();
                }
                break;
            case MESH_PDU_TYPE_SEGMENTED:
                message_pdu = (mesh_segmented_pdu_t *) pdu;
                uint8_t ctl = mesh_message_ctl(message_pdu);
                if (ctl){
                    incoming_control_pdu=  &incoming_pdu_singleton.control;
                    incoming_control_pdu->pdu_header.pdu_type = MESH_PDU_TYPE_CONTROL;

                    // flatten
                    mesh_segmented_pdu_flatten(&message_pdu->segments, 8, incoming_control_pdu->data);

                    // copy meta data into encrypted pdu buffer
                    incoming_control_pdu->len =  message_pdu->len;
                    incoming_control_pdu->netkey_index =  message_pdu->netkey_index;
                    incoming_control_pdu->akf_aid_control =  message_pdu->akf_aid_control;
                    incoming_control_pdu->flags = 0;
                    (void)memcpy(incoming_control_pdu->network_header, message_pdu->network_header, 9);

                    mesh_print_hex("Assembled payload", incoming_control_pdu->data, incoming_control_pdu->len);

                    // free mesh message
                    mesh_lower_transport_message_processed_by_higher_layer((mesh_pdu_t *)message_pdu);

                    btstack_assert(mesh_control_message_handler != NULL);
                    mesh_pdu_t * pdu = (mesh_pdu_t*) incoming_control_pdu;
                    mesh_access_message_handler(MESH_TRANSPORT_PDU_RECEIVED, MESH_TRANSPORT_STATUS_SUCCESS, pdu);

                } else {

                    incoming_access_encrypted = (mesh_pdu_t *) message_pdu;

                    incoming_access_pdu_encrypted = &incoming_access_pdu_encrypted_singleton;
                    incoming_access_pdu_encrypted->pdu_header.pdu_type = MESH_PDU_TYPE_ACCESS;
                    incoming_access_pdu_decrypted = &incoming_pdu_singleton.access;

                    // flatten
                    mesh_segmented_pdu_flatten(&message_pdu->segments, 12, incoming_access_pdu_encrypted->data);

                    // copy meta data into encrypted pdu buffer
                    incoming_access_pdu_encrypted->len =  message_pdu->len;
                    incoming_access_pdu_encrypted->netkey_index =  message_pdu->netkey_index;
                    incoming_access_pdu_encrypted->transmic_len =  message_pdu->transmic_len;
                    incoming_access_pdu_encrypted->akf_aid_control =  message_pdu->akf_aid_control;
                    (void)memcpy(incoming_access_pdu_encrypted->network_header, message_pdu->network_header, 9);

                    mesh_print_hex("Assembled payload", incoming_access_pdu_encrypted->data, incoming_access_pdu_encrypted->len);

                    // get encoded transport pdu and start processing
                    mesh_upper_transport_process_segmented_message();
                }
                break;
            default:
                btstack_assert(0);
                break;
        }
    }

    while (!btstack_linked_list_empty(&upper_transport_outgoing)){

        if (crypto_active) break;

        mesh_pdu_t * pdu =  (mesh_pdu_t *) btstack_linked_list_get_first_item(&upper_transport_outgoing);
        if (mesh_lower_transport_can_send_to_dest(mesh_pdu_dst(pdu)) == 0) break;

        mesh_upper_transport_pdu_t * upper_pdu;
        mesh_segmented_pdu_t * segmented_pdu;
        bool ok;

        switch (pdu->pdu_type){
            case MESH_PDU_TYPE_UPPER_UNSEGMENTED_CONTROL:
                // control pdus can go through directly
                btstack_assert(mesh_pdu_ctl(pdu) != 0);
                (void) btstack_linked_list_pop(&upper_transport_outgoing);
                mesh_upper_transport_send_unsegmented_control_pdu((mesh_network_pdu_t *) pdu);
                break;
            case MESH_PDU_TYPE_UPPER_SEGMENTED_CONTROL:
                // control pdus can go through directly
                btstack_assert(mesh_pdu_ctl(pdu) != 0);
                (void) btstack_linked_list_pop(&upper_transport_outgoing);
                mesh_upper_transport_send_segmented_control_pdu((mesh_upper_transport_pdu_t *) pdu);
                break;
            case MESH_PDU_TYPE_UPPER_SEGMENTED_ACCESS:
                // segmented access pdus required a mesh-segmented-pdu
                upper_pdu = (mesh_upper_transport_pdu_t *) pdu;
                if (upper_pdu->lower_pdu == NULL){
                    segmented_pdu = btstack_memory_mesh_segmented_pdu_get();
                }
                if (segmented_pdu == NULL) break;
                upper_pdu->lower_pdu = (mesh_pdu_t *) segmented_pdu;
                segmented_pdu->pdu_header.pdu_type = MESH_PDU_TYPE_SEGMENTED;
                // and a mesh-network-pdu for each segment in upper pdu
                ok = mesh_segmented_allocate_segments(&segmented_pdu->segments, upper_pdu->len + upper_pdu->transmic_len);
                if (!ok) break;
                // all buffers available, get started
                (void) btstack_linked_list_pop(&upper_transport_outgoing);
                mesh_upper_transport_send_access(upper_pdu);
                break;
            case MESH_PDU_TYPE_UPPER_UNSEGMENTED_ACCESS:
                // unsegmented access pdus require a single mesh-network-dpu
                upper_pdu = (mesh_upper_transport_pdu_t *) pdu;
                if (upper_pdu->lower_pdu == NULL){
                    upper_pdu->lower_pdu = (mesh_pdu_t *) mesh_network_pdu_get();
                }
                if (upper_pdu->lower_pdu == NULL) break;
                (void) btstack_linked_list_pop(&upper_transport_outgoing);
                mesh_upper_transport_send_access((mesh_upper_transport_pdu_t *) pdu);
                break;
            default:
                btstack_assert(false);
                break;
        }
    }
}

static mesh_upper_transport_pdu_t * mesh_upper_transport_find_pdu_for_lower(mesh_pdu_t * pdu_to_find){
    btstack_linked_list_iterator_t it;
    btstack_linked_list_iterator_init(&it, &upper_transport_outgoing_active);
    mesh_upper_transport_pdu_t * upper_pdu;
    while (btstack_linked_list_iterator_has_next(&it)){
        mesh_pdu_t * mesh_pdu = (mesh_pdu_t *) btstack_linked_list_iterator_next(&it);
        switch (mesh_pdu->pdu_type){
            case MESH_PDU_TYPE_UPPER_SEGMENTED_CONTROL:
            case MESH_PDU_TYPE_UPPER_UNSEGMENTED_ACCESS:
            case MESH_PDU_TYPE_UPPER_SEGMENTED_ACCESS:
                upper_pdu = (mesh_upper_transport_pdu_t *) mesh_pdu;
                if (upper_pdu->lower_pdu == pdu_to_find){
                    btstack_linked_list_iterator_remove(&it);
                    return upper_pdu;
                }
                break;
            default:
                break;
        }
    }
    return NULL;
}

static void mesh_upper_transport_pdu_handler(mesh_transport_callback_type_t callback_type, mesh_transport_status_t status, mesh_pdu_t * pdu){
    mesh_upper_transport_pdu_t * upper_pdu;
    mesh_network_pdu_t * network_pdu;
    mesh_segmented_pdu_t * segmented_pdu;
    switch (callback_type){
        case MESH_TRANSPORT_PDU_RECEIVED:
            mesh_upper_transport_message_received(pdu);
            break;
        case MESH_TRANSPORT_PDU_SENT:
            switch (pdu->pdu_type){
                case MESH_PDU_TYPE_SEGMENTED:
                    // try to find in outgoing active
                    upper_pdu = mesh_upper_transport_find_pdu_for_lower(pdu);
                    btstack_assert(upper_pdu != NULL);
                    segmented_pdu = (mesh_segmented_pdu_t *) pdu;
                    // free chunks
                    while (!btstack_linked_list_empty(&segmented_pdu->segments)){
                        mesh_network_pdu_t * network_pdu = (mesh_network_pdu_t *) btstack_linked_list_pop(&segmented_pdu->segments);
                        mesh_network_pdu_free(network_pdu);
                    }
                    // free segmented pdu
                    btstack_memory_mesh_segmented_pdu_free(segmented_pdu);
                    // TODO: free segmented_pdu
                    upper_pdu->lower_pdu = NULL;
                    switch (upper_pdu->pdu_header.pdu_type){
                        case MESH_PDU_TYPE_UPPER_SEGMENTED_CONTROL:
                            mesh_control_message_handler(callback_type, status, (mesh_pdu_t *) upper_pdu);
                            break;
                        case MESH_PDU_TYPE_UPPER_SEGMENTED_ACCESS:
                            mesh_access_message_handler(callback_type, status, (mesh_pdu_t *) upper_pdu);
                            break;
                        default:
                            btstack_assert(false);
                            break;
                    }
                    break;
                case MESH_PDU_TYPE_UPPER_UNSEGMENTED_ACCESS:
                    // find corresponding upper transport pdu and free single segment
                    upper_pdu = mesh_upper_transport_find_pdu_for_lower(pdu);
                    btstack_assert(upper_pdu != NULL);
                    btstack_assert(upper_pdu->lower_pdu == (mesh_pdu_t *) pdu);
                    mesh_network_pdu_free((mesh_network_pdu_t *) pdu);
                    upper_pdu->lower_pdu = NULL;
                    mesh_access_message_handler(callback_type, status, (mesh_pdu_t*) upper_pdu);
                    break;
                case MESH_PDU_TYPE_UPPER_UNSEGMENTED_CONTROL:
                    mesh_access_message_handler(callback_type, status, pdu);
                    break;
                default:
                    btstack_assert(false);
                    break;
            }
            mesh_upper_transport_run();
            break;
        default:
            break;
    }
}

void mesh_upper_transport_pdu_free(mesh_pdu_t * pdu){
    mesh_network_pdu_t   * network_pdu;
    mesh_segmented_pdu_t   * message_pdu;
    switch (pdu->pdu_type) {
        case MESH_PDU_TYPE_NETWORK:
            network_pdu = (mesh_network_pdu_t *) pdu;
            mesh_network_pdu_free(network_pdu);
            break;
        case MESH_PDU_TYPE_SEGMENTED:
            message_pdu = (mesh_segmented_pdu_t *) pdu;
            mesh_message_pdu_free(message_pdu);
        default:
            btstack_assert(false);
            break;
    }
}

void mesh_upper_transport_message_processed_by_higher_layer(mesh_pdu_t * pdu){
    crypto_active = 0;
    switch (pdu->pdu_type){
        case MESH_PDU_TYPE_ACCESS:
            mesh_upper_transport_process_access_message_done((mesh_access_pdu_t *) pdu);
        case MESH_PDU_TYPE_CONTROL:
            mesh_upper_transport_process_control_message_done((mesh_control_pdu_t *) pdu);
            break;
        default:
            btstack_assert(0);
            break;
    }
}

void mesh_upper_transport_send_access_pdu(mesh_pdu_t *pdu){
    switch (pdu->pdu_type){
        case MESH_PDU_TYPE_UPPER_SEGMENTED_ACCESS:
        case MESH_PDU_TYPE_UPPER_UNSEGMENTED_ACCESS:
            break;
        default:
            btstack_assert(false);
            break;
    }

    btstack_assert(((mesh_upper_transport_pdu_t *) pdu)->lower_pdu == NULL);

    btstack_linked_list_add_tail(&upper_transport_outgoing, (btstack_linked_item_t*) pdu);
    mesh_upper_transport_run();
}

void mesh_upper_transport_send_control_pdu(mesh_pdu_t * pdu){
    switch (pdu->pdu_type){
        case MESH_PDU_TYPE_UPPER_SEGMENTED_CONTROL:
            break;
        case MESH_PDU_TYPE_UPPER_UNSEGMENTED_CONTROL:
            btstack_assert( ((mesh_network_pdu_t *) pdu)->len >= 9);
            break;
        default:
            btstack_assert(false);
            break;
    }

    btstack_linked_list_add_tail(&upper_transport_outgoing, (btstack_linked_item_t*) pdu);
    mesh_upper_transport_run();
}

static uint8_t mesh_upper_transport_setup_unsegmented_control_pdu(mesh_network_pdu_t * network_pdu, uint16_t netkey_index, uint8_t ttl, uint16_t src, uint16_t dest, uint8_t opcode,
                                                                  const uint8_t * control_pdu_data, uint16_t control_pdu_len){

    if (control_pdu_len > 11) return 1;

    const mesh_network_key_t * network_key = mesh_network_key_list_get(netkey_index);
    if (!network_key) return 1;

    uint8_t transport_pdu_data[12];
    transport_pdu_data[0] = opcode;
    (void)memcpy(&transport_pdu_data[1], control_pdu_data, control_pdu_len);
    uint16_t transport_pdu_len = control_pdu_len + 1;

    // setup network_pdu
    mesh_network_setup_pdu(network_pdu, netkey_index, network_key->nid, 1, ttl, 0, src, dest, transport_pdu_data, transport_pdu_len);

    return 0;
}

static uint8_t mesh_upper_transport_setup_segmented_control_pdu(mesh_upper_transport_pdu_t * upper_pdu, uint16_t netkey_index, uint8_t ttl, uint16_t src, uint16_t dest, uint8_t opcode,
                                                                const uint8_t * control_pdu_data, uint16_t control_pdu_len){

    if (control_pdu_len > 256) return 1;

    const mesh_network_key_t * network_key = mesh_network_key_list_get(netkey_index);
    if (!network_key) return 1;

    upper_pdu->ivi_nid = network_key->nid | ((mesh_get_iv_index_for_tx() & 1) << 7);
    upper_pdu->ctl_ttl = ttl;
    upper_pdu->src = src;
    upper_pdu->dst = dest;
    upper_pdu->transmic_len = 0;    // no TransMIC for control
    upper_pdu->netkey_index = netkey_index;
    upper_pdu->akf_aid_control = opcode;

    // allocate segments
    btstack_linked_list_t free_segments = NULL;
    bool ok = mesh_segmented_allocate_segments( &free_segments, control_pdu_len);
    if (!ok) return 1;
    // store control pdu
    mesh_segmented_store_payload(control_pdu_data, control_pdu_len, &free_segments, &upper_pdu->segments);
    upper_pdu->len = control_pdu_len;
    return 0;
}

uint8_t mesh_upper_transport_setup_control_pdu(mesh_pdu_t * pdu, uint16_t netkey_index,
                                               uint8_t ttl, uint16_t src, uint16_t dest, uint8_t opcode, const uint8_t * control_pdu_data, uint16_t control_pdu_len){
    switch (pdu->pdu_type){
        case MESH_PDU_TYPE_UPPER_UNSEGMENTED_CONTROL:
            return mesh_upper_transport_setup_unsegmented_control_pdu((mesh_network_pdu_t *) pdu, netkey_index, ttl, src, dest, opcode, control_pdu_data, control_pdu_len);
        case MESH_PDU_TYPE_UPPER_SEGMENTED_CONTROL:
            return mesh_upper_transport_setup_segmented_control_pdu((mesh_upper_transport_pdu_t *) pdu,  netkey_index, ttl, src, dest, opcode, control_pdu_data, control_pdu_len);
        default:
            btstack_assert(0);
            return 1;
    }
}

static uint8_t mesh_upper_transport_setup_segmented_access_pdu_header(mesh_access_pdu_t * access_pdu, uint16_t netkey_index,
                                                                      uint16_t appkey_index, uint8_t ttl, uint16_t src, uint16_t dest, uint8_t szmic){

    // get app or device key
    const mesh_transport_key_t *appkey;
    appkey = mesh_transport_key_get(appkey_index);
    if (appkey == NULL) {
        printf("[!] Upper transport, setup segmented Access PDU - appkey_index %x unknown\n", appkey_index);
        return 1;
    }
    uint8_t akf_aid = (appkey->akf << 6) | appkey->aid;

    // lookup network by netkey_index
    const mesh_network_key_t *network_key = mesh_network_key_list_get(netkey_index);
    if (!network_key) return 1;
    if (network_key == NULL) {
        printf("[!] Upper transport, setup segmented Access PDU - netkey_index %x unknown\n", appkey_index);
        return 1;
    }

    const uint8_t trans_mic_len = szmic ? 8 : 4;

    // store in transport pdu
    access_pdu->transmic_len = trans_mic_len;
    access_pdu->netkey_index = netkey_index;
    access_pdu->appkey_index = appkey_index;
    access_pdu->akf_aid_control = akf_aid;
    mesh_access_set_nid_ivi(access_pdu, network_key->nid | ((mesh_get_iv_index_for_tx() & 1) << 7));
    mesh_access_set_src(access_pdu, src);
    mesh_access_set_dest(access_pdu, dest);
    mesh_access_set_ctl_ttl(access_pdu, ttl);
    return 0;
}

static uint8_t mesh_upper_transport_setup_upper_access_pdu_header(mesh_upper_transport_pdu_t * upper_pdu, uint16_t netkey_index,
                                                                  uint16_t appkey_index, uint8_t ttl, uint16_t src, uint16_t dest, uint8_t szmic){

    // get app or device key
    const mesh_transport_key_t *appkey;
    appkey = mesh_transport_key_get(appkey_index);
    if (appkey == NULL) {
        printf("[!] Upper transport, setup segmented Access PDU - appkey_index %x unknown\n", appkey_index);
        return 1;
    }
    uint8_t akf_aid = (appkey->akf << 6) | appkey->aid;

    // lookup network by netkey_index
    const mesh_network_key_t *network_key = mesh_network_key_list_get(netkey_index);
    if (!network_key) return 1;
    if (network_key == NULL) {
        printf("[!] Upper transport, setup segmented Access PDU - netkey_index %x unknown\n", appkey_index);
        return 1;
    }

    const uint8_t trans_mic_len = szmic ? 8 : 4;

    // store in transport pdu
    upper_pdu->ivi_nid = network_key->nid | ((mesh_get_iv_index_for_tx() & 1) << 7);
    upper_pdu->ctl_ttl = ttl;
    upper_pdu->src = src;
    upper_pdu->dst = dest;
    upper_pdu->transmic_len = trans_mic_len;
    upper_pdu->netkey_index = netkey_index;
    upper_pdu->appkey_index = appkey_index;
    upper_pdu->akf_aid_control = akf_aid;
    return 0;
}

static uint8_t mesh_upper_transport_setup_upper_access_pdu(mesh_upper_transport_pdu_t * upper_pdu, uint16_t netkey_index, uint16_t appkey_index, uint8_t ttl, uint16_t src, uint16_t dest,
                                                           uint8_t szmic, const uint8_t * access_pdu_data, uint8_t access_pdu_len){
    int status = mesh_upper_transport_setup_upper_access_pdu_header(upper_pdu, netkey_index, appkey_index, ttl, src,
                                                                    dest, szmic);
    if (status) return status;

    // allocate segments
    btstack_linked_list_t free_segments = NULL;
    bool ok = mesh_segmented_allocate_segments( &free_segments, access_pdu_len);
    if (!ok) return 1;
    // store control pdu
    mesh_segmented_store_payload(access_pdu_data, access_pdu_len, &free_segments, &upper_pdu->segments);
    upper_pdu->len = access_pdu_len;
    return 0;
}


uint8_t mesh_upper_transport_setup_access_pdu_header(mesh_pdu_t * pdu, uint16_t netkey_index, uint16_t appkey_index,
                                                     uint8_t ttl, uint16_t src, uint16_t dest, uint8_t szmic){
    switch (pdu->pdu_type){
        case MESH_PDU_TYPE_ACCESS:
            return mesh_upper_transport_setup_segmented_access_pdu_header((mesh_access_pdu_t *) pdu, netkey_index, appkey_index, ttl, src, dest, szmic);
        default:
            btstack_assert(false);
            return 1;
    }
}

uint8_t mesh_upper_transport_setup_access_pdu(mesh_pdu_t * pdu, uint16_t netkey_index, uint16_t appkey_index,
                                              uint8_t ttl, uint16_t src, uint16_t dest, uint8_t szmic,
                                              const uint8_t * access_pdu_data, uint8_t access_pdu_len){
    switch (pdu->pdu_type){
        case MESH_PDU_TYPE_UPPER_SEGMENTED_ACCESS:
        case MESH_PDU_TYPE_UPPER_UNSEGMENTED_ACCESS:
            return mesh_upper_transport_setup_upper_access_pdu((mesh_upper_transport_pdu_t *) pdu, netkey_index,
                                                               appkey_index, ttl, src, dest, szmic, access_pdu_data,
                                                               access_pdu_len);
        default:
            btstack_assert(false);
            return 1;
    }
}

void mesh_upper_transport_register_access_message_handler(void (*callback)(mesh_transport_callback_type_t callback_type, mesh_transport_status_t status, mesh_pdu_t * pdu)) {
    mesh_access_message_handler = callback;
}

void mesh_upper_transport_register_control_message_handler(void (*callback)(mesh_transport_callback_type_t callback_type, mesh_transport_status_t status, mesh_pdu_t * pdu)){
    mesh_control_message_handler = callback;
}

void mesh_upper_transport_init(){
    mesh_lower_transport_set_higher_layer_handler(&mesh_upper_transport_pdu_handler);
}
