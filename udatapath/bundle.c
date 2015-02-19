/* Copyright (c) 2011, TrafficLab, Ericsson Research, Hungary
 * Copyright (c) 2012, CPqD, Brazil
 * Copyright (c) 2013, Netronome Systems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of the Ericsson Research nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <stdbool.h>
#include <string.h>
#include "dynamic-string.h"
#include "datapath.h"
#include "bundle.h"
#include "oflib/ofl.h"
#include "time.h"
#include "dp_capabilities.h"
#include "dp_control.h"

#include "vlog.h"

extern uint32_t bundle_time_offset_g; //ORON
extern struct bundle_time_ctl bundle_time_ctl;//TIME_EXTENTION_EXP

#define LOG_MODULE VLM_bundle
static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(60, 60);

/* Create a structure to retain an appended message. */
static struct bundle_message *
bundle_message_create(struct ofl_msg_bundle_add_msg *add_msg) {
    struct bundle_message *msg;
    size_t message_length;

    msg = xmalloc(sizeof(struct bundle_message));
    list_init(&msg->node);

    message_length = ntohs(add_msg->message->length);
    msg->message = xmalloc(message_length);
    memcpy(msg->message, add_msg->message, message_length);

    VLOG_DBG_RL(LOG_MODULE, &rl,
           "Created %u byte bundle message entry for bundle ID %u.\n",
           message_length,
           add_msg->bundle_id);

    return msg;
}

/* Free an appended message structure. */
static void
bundle_message_free(struct bundle_message *msg) {
    free(msg->message);
    free(msg);
}

/* Create the table entry for the specified bundle ID. */
static struct bundle_table_entry *
bundle_table_entry_create(uint32_t bundle_id, uint16_t flags) {
    struct bundle_table_entry *entry;

    entry = xmalloc(sizeof(struct bundle_table_entry));
    list_init(&entry->node);
    list_init(&entry->bundle_message_list);
    entry->bundle_id = bundle_id;
    entry->flags = flags;
    entry->closed = false;
    VLOG_DBG_RL(LOG_MODULE, &rl, "Created bundle table entry for bundle ID %u.\n", bundle_id);

    return entry;
}

/* Free the specified entry which keeps state for one bundle ID. */
static void
bundle_table_entry_destroy(struct bundle_table_entry *entry) {
    struct bundle_message *bundle_msg, *bundle_msg_next;

    LIST_FOR_EACH_SAFE (bundle_msg, bundle_msg_next, struct bundle_message, node, &entry->bundle_message_list) {
        VLOG_DBG_RL(LOG_MODULE, &rl, "Free message with type %u and length %u\n",
               bundle_msg->message->type,
               ntohs(bundle_msg->message->length));
        list_remove(&bundle_msg->node);
        bundle_message_free(bundle_msg);
    }
    list_remove(&entry->node);
    VLOG_DBG_RL(LOG_MODULE, &rl, "Destroyed bundle table entry for bundle ID %u.\n", entry->bundle_id);
    free(entry);
}

/* Create a bundle state table (covering multiple bundle IDs). */
struct bundle_table *
bundle_table_create(struct datapath *dp) {
    struct bundle_table *table;

    table = xmalloc(sizeof(struct bundle_table));
    table->dp = dp;
    list_init(&table->bundle_table_entries);

    return table;
}

/* Destroy a bundle state table. */
void
bundle_table_destroy(struct bundle_table *table) {
    struct bundle_table_entry *entry, *next;
    LIST_FOR_EACH_SAFE (entry, next, struct bundle_table_entry, node, &table->bundle_table_entries) {
        bundle_table_entry_destroy(entry);
    }
    free(table);
}

/* Find the bundle table entry corresponding to the specified ID. */
static struct bundle_table_entry *
bundle_table_entry_find(struct bundle_table *table,
                        uint32_t bundle_id) {
    struct bundle_table_entry *entry;

    LIST_FOR_EACH (entry, struct bundle_table_entry, node, &table->bundle_table_entries) {
        if (entry->bundle_id == bundle_id) {
            return entry;
        }
    }

    return NULL;
}

/* Open operation. */
static ofl_err
bundle_open(struct bundle_table *table,
            uint32_t bundle_id, uint16_t flags) {
    struct bundle_table_entry *entry;
    ofl_err error;

    entry = bundle_table_entry_find(table, bundle_id);
    if (entry != NULL) {
        error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_BUNDLE_EXIST);
        bundle_table_entry_destroy(entry);
    } else {
        entry = bundle_table_entry_create(bundle_id, flags);
        list_push_back(&table->bundle_table_entries, &entry->node);
        error = 0;
    }

    return error;
}

/* Close operation. */
static ofl_err
bundle_close(struct bundle_table *table,
             uint32_t bundle_id, uint16_t flags) {
    struct bundle_table_entry *entry;
    ofl_err error;

    entry = bundle_table_entry_find(table, bundle_id);
    if (entry == NULL) {
        error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_BAD_ID);
    } else {
        if (entry->flags != flags) {
            error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_BAD_FLAGS);
            bundle_table_entry_destroy(entry);
        } else if (entry->closed) {
            error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_BUNDLE_CLOSED);
            bundle_table_entry_destroy(entry);
        } else {
            /* TODO check bundled messages (e.g. syntax / parameter check
             * and perform dry run of execution) to gain more confidence
             * that commit will succeed, return error if any issues found */

            /* Mark closed */
            entry->closed = true;
            error = 0;
        }
    }

    return error;
}

/* Discard operation. */
static ofl_err
bundle_discard(struct bundle_table *table,
               uint32_t bundle_id, uint16_t flags) {
    struct bundle_table_entry *entry;
    ofl_err error;
    //TIME_EXTENTION_EXP(open)
    if(bundle_time_ctl.commit_msg[bundle_id].discard==0){ //ORON
    	bundle_time_ctl.commit_msg[bundle_id].discard = 1; //ORON
    }
    //TIME_EXTENTION_EXP(close)
    entry = bundle_table_entry_find(table, bundle_id);
    if (entry != NULL) {
	/* Spec says we should always succeed, so don't fail on bad flags. */
        error = 0;
        bundle_table_entry_destroy(entry);
    } else {
        error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_BAD_ID);
    }

    return error;
}

/* ADD_MESSAGE message operation. */
static ofl_err
bundle_add_msg(struct bundle_table *table, struct ofl_msg_bundle_add_msg *add_msg) {
    struct bundle_table_entry *entry;
    struct bundle_message *new_message;
    ofl_err error = 0;

    entry = bundle_table_entry_find(table, add_msg->bundle_id);

    if (entry != NULL) {
        if (entry->flags != add_msg->flags) {
            error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_BAD_FLAGS);
            bundle_table_entry_destroy(entry);
        } else if (entry->closed) {
            error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_BUNDLE_CLOSED);
            bundle_table_entry_destroy(entry);
        }
    } else {
        entry = bundle_table_entry_create(add_msg->bundle_id, add_msg->flags);
        list_push_back(&table->bundle_table_entries, &entry->node);
    }

    if (!error) {
        new_message = bundle_message_create(add_msg);
        list_push_back(&entry->bundle_message_list, &new_message->node);
    }

    return error;
}

/* Commit operation. */
static ofl_err
bundle_commit(struct datapath *dp,
              struct bundle_table *table,
              uint32_t bundle_id,
              uint16_t flags,
              const struct sender *sender) {
    struct bundle_table_entry *entry;
    struct bundle_message *bundle_msg;
    struct ofl_msg_header *msg;
    uint32_t xid;
    ofl_err error;
    ofl_err last_error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_BAD_ID);

    /* Find and process commit operation for bundle ID */
    entry = bundle_table_entry_find(table, bundle_id);
    if (entry != NULL) {
        /* Ensure flags are consistent with flags specified previously */
        if (entry->flags != flags) {
            last_error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_BAD_FLAGS);
        } else {
            /* Save state in case failure occurs */
            last_error = 0;

            dp_save_state(dp);

            /* Commit all messages in bundle, stopping at first error */
            LIST_FOR_EACH (bundle_msg, struct bundle_message, node, &entry->bundle_message_list) {
                VLOG_DBG_RL(LOG_MODULE, &rl, "Commit of message with type %u and length %u\n",
                       bundle_msg->message->type,
                       ntohs(bundle_msg->message->length));

                error = ofl_msg_unpack((uint8_t *)bundle_msg->message,
                                       ntohs(bundle_msg->message->length),
                                       &msg, &xid, dp->exp);

                if (!error) {
                    /* This prototype only properly supports bundling of
                     * messages that do not generate replies (other than
                     * error replies).  TODO: keep replies in a holding
                     * area and only release them to the controller when
                     * the commit succeeds. */
                    error = handle_control_msg(dp, msg, sender);

                    if (error) {
                        ofl_msg_free(msg, dp->exp);
                    }
                }

                if (error) {
                    last_error = error;
                    break;
                }
            }

            /* Restore state if failures occurred */
            if (last_error) {
                dp_restore_state(dp);
            } else {
                /* TODO free memory used to save state without restoring
                 * (not required currently as variables used to save/restore
                 * state are re-used) */
            }

	    /* We need to generate the error ourselves. The spec say that
	     * the error need to refer to the offending message in the budle.
	     * If we just return the error code, the error message would refer
	     * to the commit message. */
            if (last_error) {
                struct sender orig_sender = {.remote = sender->remote,
					     .conn_id = sender->conn_id,
					     .xid = xid};

		struct ofl_msg_error orig_err =
                            {{.type = OFPT_ERROR},
                             .type = ofl_error_type(last_error),
                             .code = ofl_error_code(last_error),
                             .data_length = ntohs(bundle_msg->message->length),
                             .data        = (uint8_t *)bundle_msg->message};
		dp_send_message(dp, (struct ofl_msg_header *)&orig_err, &orig_sender);
		/* Trigger second error message. */
		last_error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_MSG_FAILED);
	    }
        }

        /* Whether or not commit succeeded: free entry for bundle ID */
        bundle_table_entry_destroy(entry);
    }

    return last_error;
}
//TIME_EXTENTION_EXP(open)
ofl_err
bundle_handle_features_request(struct datapath *dp,
        struct bundle_table *table,
        struct ofl_msg_multipart_request_bundle_features *req,
        const struct sender *sender) {

    struct timeval time_check;

	struct ofl_msg_multipart_relpy_bundle_features reply=
    {{{.type = OFPT_MULTIPART_REPLY},
      .type = OFPMP_BUNDLE_FEATURES, .flags = 0x0000},.capabilities=0};

	// TIMESTAMP FLAG recievied
	if((req->feature_request_flags & OFPBF_TIMESTAMP) >0){
		//request include timestamp of controller send time
	}
	// always returns reply timestamp
	gettimeofday(&time_check, 0);
	reply.features.timestamp.seconds            = time_check.tv_sec + bundle_time_offset_g;
	reply.features.timestamp.nanoseconds        = time_check.tv_usec*1000;


	// SET SCHED FLAG
	if((req->feature_request_flags & OFPBF_TIME_SET_SCHED) >0){
		bundle_time_ctl.features.sched_accuracy.seconds       = req->features.sched_accuracy.seconds;
		bundle_time_ctl.features.sched_accuracy.nanoseconds   = req->features.sched_accuracy.nanoseconds;
		bundle_time_ctl.features.sched_max_future.seconds     = req->features.sched_max_future.seconds;
		bundle_time_ctl.features.sched_max_future.nanoseconds = req->features.sched_max_future.nanoseconds;
		bundle_time_ctl.features.sched_max_past.seconds       = req->features.sched_max_past.seconds;
		bundle_time_ctl.features.sched_max_past.nanoseconds   = req->features.sched_max_past.nanoseconds;
	}

	reply.capabilities = bundle_time_ctl.capabilities;
	reply.features.type = OFPTMPBF_TIME_CAPABILITY;
	reply.features.sched_accuracy.seconds       = bundle_time_ctl.features.sched_accuracy.seconds;
	reply.features.sched_accuracy.nanoseconds   = bundle_time_ctl.features.sched_accuracy.nanoseconds;
	reply.features.sched_max_future.seconds     = bundle_time_ctl.features.sched_max_future.seconds;
	reply.features.sched_max_future.nanoseconds = bundle_time_ctl.features.sched_max_future.nanoseconds;
	reply.features.sched_max_past.seconds       = bundle_time_ctl.features.sched_max_past.seconds;
	reply.features.sched_max_past.nanoseconds   = bundle_time_ctl.features.sched_max_past.nanoseconds;;


    if(sender->remote->role == OFPCR_ROLE_SLAVE) {
        return ofl_error(OFPET_BAD_REQUEST, OFPBRC_IS_SLAVE);
    }
    else{
    	dp_send_message(dp, (struct ofl_msg_header *)&reply, sender);
        ofl_msg_free((struct ofl_msg_header *)req, dp->exp);
    }
	return 0;
}
//TIME_EXTENTION_EXP(close)
/* Handle bundle control operations: open, close, discard, commit. */
ofl_err
bundle_handle_control(struct datapath *dp,
                      struct bundle_table *table,
                      struct ofl_msg_bundle_control *ctl,
                      const struct sender *sender) {
	struct ofp_bundle_prop_time *prop_time;//TIME_EXTENTION_EXP
    struct timeval time_check; //TIME_EXTENTION_EXP
    uint64_t max_future_sec, max_past_sec;
    uint32_t max_future_nan, max_past_nan;
    struct ofl_msg_bundle_control reply =
            {{.type = OFPT_BUNDLE_CONTROL}};
    ofl_err error;

    if(sender->remote->role == OFPCR_ROLE_SLAVE) {
        return ofl_error(OFPET_BAD_REQUEST, OFPBRC_IS_SLAVE);
    }

    VLOG_DBG_RL(LOG_MODULE, &rl, "Processing bundle control message with type %d\n", ctl->type);
    switch (ctl->type) {
        case OFPBCT_OPEN_REQUEST: {
            VLOG_DBG_RL(LOG_MODULE, &rl, "Processing bundle open of bundle ID %u\n", ctl->bundle_id);
            error = bundle_open(table, ctl->bundle_id, ctl->flags);
            if(!error) {
                reply.type = OFPBCT_OPEN_REPLY;
                reply.bundle_id = ctl->bundle_id;
                dp_send_message(dp, (struct ofl_msg_header *)&reply, sender);
                ofl_msg_free((struct ofl_msg_header *)ctl, dp->exp);
            }
            return error;
        }
        case OFPBCT_CLOSE_REQUEST: {
            VLOG_DBG_RL(LOG_MODULE, &rl, "Processing bundle close of bundle ID %u\n", ctl->bundle_id);
            error = bundle_close(table, ctl->bundle_id, ctl->flags);
            if(!error) {
                reply.type = OFPBCT_CLOSE_REPLY;
                reply.bundle_id = ctl->bundle_id;
                dp_send_message(dp, (struct ofl_msg_header *)&reply, sender);
                ofl_msg_free((struct ofl_msg_header *)ctl, dp->exp);
            }
            return error;
        }
        case OFPBCT_DISCARD_REQUEST: {
            VLOG_DBG_RL(LOG_MODULE, &rl, "Processing bundle discard of bundle ID %u\n", ctl->bundle_id);
            error = bundle_discard(table, ctl->bundle_id, ctl->flags);
            if(!error) {
            	bundle_time_ctl.commit_msg[ctl->bundle_id].ctl.flags=0; //TIME_EXTENTION_EXP (discard any bundle pending) //ORON
                reply.type = OFPBCT_DISCARD_REPLY;
                reply.bundle_id = ctl->bundle_id;
                dp_send_message(dp, (struct ofl_msg_header *)&reply, sender);
                ofl_msg_free((struct ofl_msg_header *)ctl, dp->exp);
            }
            return error;
        }
        case OFPBCT_COMMIT_REQUEST: {
        	switch (ctl->flags){
        	//TIME_EXTENTION_EXP(open)
        		case OFPBF_TIME:{
        		error = 0;
				VLOG_DBG_RL(LOG_MODULE, &rl, "Processing bundle commit IN TIME of bundle ID %u, no ACK on this msg (if not error)\n", ctl->bundle_id);
					prop_time = (struct ofp_bundle_prop_time *)ctl->properties;
					bundle_time_ctl.commit_msg[ctl->bundle_id].sched_time.nanoseconds = prop_time->scheduled_time.nanoseconds; //ORON
					bundle_time_ctl.commit_msg[ctl->bundle_id].sched_time.seconds     = prop_time->scheduled_time.seconds; //ORON
					bundle_time_ctl.commit_msg[ctl->bundle_id].discard                = 0;
					// Check if commit time is valid
					gettimeofday(&time_check, 0);
					time_check.tv_sec += bundle_time_offset_g;
					// check overflow and compute time limits
					max_future_sec = 0;
					max_future_nan = time_check.tv_usec*1000 + bundle_time_ctl.features.sched_max_future.nanoseconds;
					if(max_future_nan>=1000000000){
						max_future_nan -= 1000000000;
						max_future_sec += 1;
					}
					max_future_sec += time_check.tv_sec + bundle_time_ctl.features.sched_max_future.seconds;

					max_past_sec   = 0;
					if(time_check.tv_usec*1000 < bundle_time_ctl.features.sched_max_past.nanoseconds){
						max_past_nan  = 1000000000 - bundle_time_ctl.features.sched_max_past.nanoseconds;
						max_past_sec  = time_check.tv_sec - bundle_time_ctl.features.sched_max_past.seconds - 1;
					}
					else{
						max_past_nan =  time_check.tv_usec*1000 - bundle_time_ctl.features.sched_max_past.nanoseconds;
						max_past_sec =  time_check.tv_sec - bundle_time_ctl.features.sched_max_past.seconds;
					}
					//check time limits
					//future limit check
					//ORON
					VLOG_DBG_RL(LOG_MODULE, &rl, "future check : %ld vs %ld (%ld - %ld)", bundle_time_ctl.features.sched_max_future.seconds, bundle_time_ctl.commit_msg[ctl->bundle_id].sched_time.seconds - time_check.tv_sec, bundle_time_ctl.commit_msg[ctl->bundle_id].sched_time.seconds, time_check.tv_sec); //ORON
					if(max_future_sec < bundle_time_ctl.commit_msg[ctl->bundle_id].sched_time.seconds){
						error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_SCHED_FUTURE);
					}
					else if(max_future_sec == bundle_time_ctl.commit_msg[ctl->bundle_id].sched_time.seconds){
						if(max_future_nan < bundle_time_ctl.commit_msg[ctl->bundle_id].sched_time.nanoseconds){
							error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_SCHED_FUTURE);
						}
					}
					//past limit check
					if(max_past_sec > bundle_time_ctl.commit_msg[ctl->bundle_id].sched_time.seconds){
						error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_SCHED_PAST);
					}
					else if (max_past_sec == bundle_time_ctl.commit_msg[ctl->bundle_id].sched_time.seconds){
						if(max_past_nan > bundle_time_ctl.commit_msg[ctl->bundle_id].sched_time.nanoseconds){
							error = ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_SCHED_PAST);
						}
					}

					if(error){
						return error;
					}
					bundle_time_ctl.commit_msg[ctl->bundle_id].ctl=*ctl;
					bundle_time_ctl.commit_msg[ctl->bundle_id].table = table;
					bundle_time_ctl.commit_msg[ctl->bundle_id].remote=sender->remote;
					bundle_time_ctl.commit_msg[ctl->bundle_id].conn_id=sender->conn_id;
					bundle_time_ctl.commit_msg[ctl->bundle_id].xid=sender->xid;
						ofl_msg_free((struct ofl_msg_header *)ctl, dp->exp);//TIME_EXTENTION_EXP ? maybe
						return error; // 0
				break;
				}
        		// normal commit (now)
        		default:{
					VLOG_DBG_RL(LOG_MODULE, &rl, "Processing bundle commit of bundle ID %u\n", ctl->bundle_id);
					error = bundle_commit(dp, table, ctl->bundle_id, ctl->flags, sender);
					if(!error) {
						reply.type = OFPBCT_COMMIT_REPLY;
						reply.bundle_id = ctl->bundle_id;
						dp_send_message(dp, (struct ofl_msg_header *)&reply, sender);
						if(!bundle_time_ctl.commit_msg[ctl->bundle_id].commiting_now){ //ORON
							ofl_msg_free((struct ofl_msg_header *)ctl, dp->exp);
						}
					}
					return error;
        		}
        	}
        	break;
        }
        //TIME_EXTENTION_EXP(close)
        default: {
            return ofl_error(OFPET_BUNDLE_FAILED, OFPBFC_BAD_TYPE);
        }
    }
}

/* Handle bundle add_msg operation. */
ofl_err
bundle_handle_add_msg(struct bundle_table *table,
                     struct ofl_msg_bundle_add_msg *add_msg,
                     const struct sender *sender) {
    if(sender->remote->role == OFPCR_ROLE_SLAVE)
        return ofl_error(OFPET_BAD_REQUEST, OFPBRC_IS_SLAVE);

    return bundle_add_msg(table, add_msg);
}
