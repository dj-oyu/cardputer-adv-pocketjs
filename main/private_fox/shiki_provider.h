#ifndef FOX_SHIKI_PROVIDER_H
#define FOX_SHIKI_PROVIDER_H
#include "render_adapter.h"
#ifdef FOX_SHIKI_PROVIDER
/* Single owner only, after display join. This module borrows published leases;
 * the publisher still owns lease_retire. All pools/payloads/scratch outlive close.
 * offer preflights a staged token; accept after Shiki publish success, reject on
 * publish failure. Failed offers preserve the logical committed/pending token.
 * Only one exclusive scratch is supported while any token is held.
 * rows writes into the caller's background, no framebuffer or transport here.
 * On render failure dst MAY be modified: caller MUST NOT submit those rows.
 * ack(false) retains pending for retry. close must run only after display join.
 * Caller must not mutate scratch or borrow it elsewhere while provider active.
 */
/* Shiki owner admission; defined in pocket_shiki_frame.c. */
bool pocket_proc_fox_publish(lease_pool*,lease_key,render_scratch*);
bool fox_shiki_offer(lease_pool*,lease_key,render_scratch*);
void fox_shiki_accept(void);
void fox_shiki_reject(void);
bool fox_shiki_rows(uint16_t*,unsigned,unsigned);
void fox_shiki_ack(bool);
void fox_shiki_close(void);
bool fox_shiki_active(void);
bool fox_shiki_pending(void);
#endif
#endif
