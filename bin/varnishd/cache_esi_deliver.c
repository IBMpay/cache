/*-
 * Copyright (c) 2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * VED - Varnish Esi Delivery
 */

#include "config.h"

#include "svnid.h"
SVNID("$Id")

#include <stdlib.h>
#include <stdio.h>

#include "cache.h"
#include "cache_esi.h"
#include "vend.h"
#include "vct.h"
#include "vgz.h"
#include "stevedore.h"

/*--------------------------------------------------------------------*/

static void
ved_include(struct sess *sp, const char *src, const char *host)
{
	struct object *obj;
	struct worker *w;
	char *ws_wm;
	unsigned sxid, res_mode;

	w = sp->wrk;

	if (WRW_Flush(w)) {
		vca_close_session(sp, "remote closed");
		return;
	}

	AZ(WRW_FlushRelease(w));

	sp->esi_level++;
	obj = sp->obj;
	sp->obj = NULL;
	res_mode = sp->wrk->res_mode;

	/* Reset request to status before we started messing with it */
	HTTP_Copy(sp->http, sp->http0);

	/* Take a workspace snapshot */
	ws_wm = WS_Snapshot(sp->ws);

	http_SetH(sp->http, HTTP_HDR_URL, src);
	if (host != NULL && *host != '\0')  {
		http_Unset(sp->http, H_Host);
		http_Unset(sp->http, H_If_Modified_Since);
		http_SetHeader(w, sp->fd, sp->http, host);
	}
	/*
	 * XXX: We should decide if we should cache the director
	 * XXX: or not (for session/backend coupling).  Until then
	 * XXX: make sure we don't trip up the check in vcl_recv.
	 */
	sp->director = NULL;
	sp->step = STP_RECV;
	http_ForceGet(sp->http);

	/* Don't do conditionals */
	sp->http->conds = 0;
	http_Unset(sp->http, H_If_Modified_Since);

	/* Client content already taken care of */
	http_Unset(sp->http, H_Content_Length);

	sxid = sp->xid;
	while (1) {
		sp->wrk = w;
		CNT_Session(sp);
		if (sp->step == STP_DONE)
			break;
		AZ(sp->wrk);
		WSL_Flush(w, 0);
		DSL(0x20, SLT_Debug, sp->id, "loop waiting for ESI");
		(void)usleep(10000);
	}
	sp->xid = sxid;
	AN(sp->wrk);
	assert(sp->step == STP_DONE);
	sp->esi_level--;
	sp->obj = obj;
	sp->wrk->res_mode = res_mode;

	/* Reset the workspace */
	WS_Reset(sp->ws, ws_wm);

	WRW_Reserve(sp->wrk, &sp->fd);
}

/*--------------------------------------------------------------------*/


//#define Debug(fmt, ...) printf(fmt, __VA_ARGS__)
#define Debug(fmt, ...) /**/

static void
ved_sendchunk(const struct sess *sp, const void *cb, ssize_t cl,
    const void *ptr, ssize_t l)
{
	char chunk[20];

	assert(l > 0);
	if (sp->wrk->res_mode & RES_CHUNKED) {
		if (cb == NULL) {
			bprintf(chunk, "%jx\r\n", (intmax_t)l);
			(void)WRW_Write(sp->wrk, chunk, -1);
		} else
			(void)WRW_Write(sp->wrk, cb, cl);
	}
	(void)WRW_Write(sp->wrk, ptr, l);
	if (sp->wrk->res_mode & RES_CHUNKED) {
		(void)WRW_Write(sp->wrk, "\r\n", -1);
		if (cb == NULL)
			(void)WRW_Flush(sp->wrk);
	}
}

static ssize_t
ved_decode_len(uint8_t **pp)
{
	uint8_t *p;
	ssize_t l;

	p = *pp;
	switch (*p & 15) {
	case 1:
		l = p[1];
		p += 2;
		break;
	case 2:
		l = vbe16dec(p + 1);
		p += 3;
		break;
	case 8:
		l = vbe64dec(p + 1);
		p += 9;
		break;
	default:
		printf("Illegal Length %d %d\n", *p, (*p & 15));
		INCOMPL();
	}
	*pp = p;
	assert(l > 0);
	return (l);
}

/*---------------------------------------------------------------------
 * If a gzip'ed ESI object includes a ungzip'ed object, we need to make
 * it looked like a gzip'ed data stream.  The official way to do so would
 * be to fire up libvgz and gzip it, but we don't, we fake it.
 *
 * First, we cannot know if it is ungzip'ed on purpose, the admin may
 * know something we don't.
 *
 * What do you mean "BS ?"
 *
 * All right then...
 *
 * The matter of the fact is that we simply will not fire up a gzip in
 * the output path because it costs too much memory and CPU, so we simply
 * wrap the data in very convenient "gzip copy-blocks" and send it down
 * the stream with a bit more overhead.
 */

static void
ved_pretend_gzip(const struct sess *sp, const uint8_t *p, ssize_t l)
{
	uint8_t buf[5];
	char chunk[20];
	uint16_t lx;

	while (l > 0) {
		if (l > 65535)
			lx = 65535;
		else
			lx = (uint16_t)l;
		buf[0] = 0;
		vle16enc(buf + 1, lx);
		vle16enc(buf + 3, ~lx);
		if (sp->wrk->res_mode & RES_CHUNKED) {
			bprintf(chunk, "%x\r\n", lx + 5);
			(void)WRW_Write(sp->wrk, chunk, -1);
		}
		(void)WRW_Write(sp->wrk, buf, sizeof buf);
		(void)WRW_Write(sp->wrk, p, lx);
		if (sp->wrk->res_mode & RES_CHUNKED)
			(void)WRW_Write(sp->wrk, "\r\n", -1);
		(void)WRW_Flush(sp->wrk);
		sp->wrk->crc = crc32(sp->wrk->crc, p, lx);
		sp->wrk->l_crc += lx;
		l -= lx;
		p += lx;
	}
}

/*---------------------------------------------------------------------
 */

static const char gzip_hdr[] = {
	0x1f, 0x8b, 0x08,
	0x00, 0x00, 0x00, 0x00,
	0x00,
	0x02, 0x03
};

void
ESI_Deliver(struct sess *sp)
{
	struct storage *st;
	uint8_t *p, *e, *q, *r;
	unsigned off;
	ssize_t l, l_icrc = 0;
	uint32_t icrc = 0;
	uint8_t tailbuf[8 + 5];
	int isgzip;
	struct vgz *vgz = NULL;
	char obuf[params->gzip_stack_buffer];
	ssize_t obufl = 0;
	size_t dl;
	const void *dp;
	int i;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	st = sp->obj->esidata;
	AN(st);
	assert(sizeof obuf >= 1024);

	obuf[0] = 0;	/* For flexelint */

	p = st->ptr;
	e = st->ptr + st->len;

	if (*p == VEC_GZ) {
		isgzip = 1;
		p++;
	} else {
		isgzip = 0;
	}

	if (sp->esi_level == 0) {
		/*
		 * Only the top level document gets to decide this.
		 */
		sp->wrk->gzip_resp = 0;
		if (isgzip && !(sp->wrk->res_mode & RES_GUNZIP)) {
			assert(sizeof gzip_hdr == 10);
			/* Send out the gzip header */
			ved_sendchunk(sp, "a\r\n", 3, gzip_hdr, 10);
			sp->wrk->l_crc = 0;
			sp->wrk->gzip_resp = 1;
			sp->wrk->crc = crc32(0L, Z_NULL, 0);
		}
	}

	if (isgzip && !sp->wrk->gzip_resp) {
		vgz = VGZ_NewUngzip(sp, "U D E");

		/* Feed a gzip header to gunzip to make it happy */
		VGZ_Ibuf(vgz, gzip_hdr, sizeof gzip_hdr);
		VGZ_Obuf(vgz, obuf, sizeof obuf);
		i = VGZ_Gunzip(vgz, &dp, &dl);
		assert(i == Z_OK || i == Z_STREAM_END);
		assert(VGZ_IbufEmpty(vgz));
		assert(dl == 0);

		obufl = 0;
	}

	st = VTAILQ_FIRST(&sp->obj->store);
	off = 0;

	while (p < e) {
		switch (*p) {
		case VEC_V1:
		case VEC_V2:
		case VEC_V8:
			l = ved_decode_len(&p);
			r = p;
			q = (void*)strchr((const char*)p, '\0');
			p = q + 1;
			if (isgzip) {
				assert(*p == VEC_C1 || *p == VEC_C2 ||
				    *p == VEC_C8);
				l_icrc = ved_decode_len(&p);
				icrc = vbe32dec(p);
				p += 4;
			}
			if (sp->wrk->gzip_resp && isgzip) {
				/*
				 * We have a gzip'ed VEC and delivers a
				 * gzip'ed ESI response.
				 */
				sp->wrk->crc = crc32_combine(
				    sp->wrk->crc, icrc, l_icrc);
				sp->wrk->l_crc += l_icrc;
				ved_sendchunk(sp, r, q - r, st->ptr + off, l);
			} else if (sp->wrk->gzip_resp) {
				/*
				 * A gzip'ed ESI response, but the VEC was
				 * not gzip'ed.
				 */
				ved_pretend_gzip(sp, st->ptr + off, l);
				off += l;
			} else if (isgzip) {
				/*
				 * A gzip'ed VEC, but ungzip'ed ESI response
				 */
				AN(vgz);
				VGZ_Ibuf(vgz, st->ptr + off, l);
				do {
					if (obufl == sizeof obuf) {
						ved_sendchunk(sp, NULL, 0,
						    obuf, obufl);
						obufl = 0;
					}
					VGZ_Obuf(vgz, obuf + obufl,
					    sizeof obuf - obufl);
					i = VGZ_Gunzip(vgz, &dp, &dl);
					assert(i == Z_OK || i == Z_STREAM_END);
					obufl += dl;
				} while (!VGZ_IbufEmpty(vgz));
			} else {
				/*
				 * Ungzip'ed VEC, ungzip'ed ESI response
				 */
				ved_sendchunk(sp, r, q - r, st->ptr + off, l);
			}
			off += l;
			break;
		case VEC_S1:
		case VEC_S2:
		case VEC_S8:
			l = ved_decode_len(&p);
			Debug("SKIP1(%d)\n", (int)l);
			off += l;
			break;
		case VEC_INCL:
			p++;
			q = (void*)strchr((const char*)p, '\0');
			AN(q);
			q++;
			r = (void*)strchr((const char*)q, '\0');
			AN(r);
			if (obufl > 0) {
				ved_sendchunk(sp, NULL, 0,
				    obuf, obufl);
				obufl = 0;
			}
			Debug("INCL [%s][%s] BEGIN\n", q, p);
			ved_include(sp, (const char*)q, (const char*)p);
			Debug("INCL [%s][%s] END\n", q, p);
			p = r + 1;
			break;
		default:
			printf("XXXX 0x%02x [%s]\n", *p, p);
			INCOMPL();
		}
	}
	if (vgz != NULL) {
		if (obufl > 0)
			ved_sendchunk(sp, NULL, 0, obuf, obufl);
		VGZ_Destroy(&vgz);
	}
	if (sp->wrk->gzip_resp && sp->esi_level == 0) {
		/* Emit a gzip literal block with finish bit set */
		tailbuf[0] = 0x01;
		tailbuf[1] = 0x00;
		tailbuf[2] = 0x00;
		tailbuf[3] = 0xff;
		tailbuf[4] = 0xff;

		/* Emit CRC32 */
		vle32enc(tailbuf + 5, sp->wrk->crc);

		/* MOD(2^32) length */
		vle32enc(tailbuf + 9, sp->wrk->l_crc);

		ved_sendchunk(sp, "d\r\n", 3, tailbuf, 13);
	}
	(void)WRW_Flush(sp->wrk);
}

/*---------------------------------------------------------------------
 * Include an object in a gzip'ed ESI object delivery
 */

static uint8_t
ved_deliver_byterange(const struct sess *sp, ssize_t low, ssize_t high)
{
	struct storage *st;
	ssize_t l, lx;
	u_char *p;

//printf("BR %jd %jd\n", low, high);
	lx = 0;
	VTAILQ_FOREACH(st, &sp->obj->store, list) {
		p = st->ptr;
		l = st->len;
//printf("[0-] %jd %jd\n", lx, lx + l);
		if (lx + l < low) {
			lx += l;
			continue;
		}
		if (lx == high)
			return (p[0]);
		assert(lx < high);
		if (lx < low) {
			p += (low - lx);
			l -= (low - lx);
			lx = low;
		}
//printf("[1-] %jd %jd\n", lx, lx + l);
		if (lx + l >= high)
			l = high - lx;
//printf("[2-] %jd %jd\n", lx, lx + l);
		assert(lx >= low && lx + l <= high);
		if (l != 0)
			ved_sendchunk(sp, NULL, 0, p, l);
		if (lx + st->len > high)
			return(p[l]);
		lx += st->len;
	}
	INCOMPL();
}

void
ESI_DeliverChild(const struct sess *sp)
{
	struct storage *st;
	struct object *obj;
	ssize_t start, last, stop, lpad;
	u_char *p, cc;
	uint32_t icrc;
	uint32_t ilen;
	uint8_t pad[6] = { 0 };

	if (!sp->obj->gziped) {
		VTAILQ_FOREACH(st, &sp->obj->store, list)
			ved_pretend_gzip(sp, st->ptr, st->len);
		return;
	}
	/*
	 * This is the interesting case: Deliver all the deflate
	 * blocks, stripping the "LAST" bit of the last one and
	 * padding it, as necessary, to a byte boundary.
	 */
	obj = sp->obj;
	CHECK_OBJ_NOTNULL(obj, OBJECT_MAGIC);
	start = obj->gzip_start;
	last = obj->gzip_last;
	stop = obj->gzip_stop;
	assert(start > 0 && start < obj->len * 8);
	assert(last > 0 && last < obj->len * 8);
	assert(stop > 0 && stop < obj->len * 8);
	assert(last >= start);
	assert(last < stop);
//printf("BITS %jd %jd %jd\n", start, last, stop);

	/* The start bit must be byte aligned. */
	AZ(start & 7);

	/*
	 * XXX: optimize for the case where the 'last'
	 * XXX: bit is in a empty copy block
	 */
	cc = ved_deliver_byterange(sp, start/8, last/8);
//printf("CC_LAST %x\n", cc);
	cc &= ~(1U << (start & 7));
	ved_sendchunk(sp, NULL, 0, &cc, 1);
	cc = ved_deliver_byterange(sp, 1 + last/8, stop/8);
//printf("CC_STOP %x (%d)\n", cc, (int)(stop & 7));
	switch((int)(stop & 7)) {
	case 0: /* xxxxxxxx */
		/* I think we have an off by one here, but that's OK */
		lpad = 0;
		break;
	case 1: /* x000.... 00000000 00000000 11111111 11111111 */
	case 3: /* xxx000.. 00000000 00000000 11111111 11111111 */
	case 5: /* xxxxx000 00000000 00000000 11111111 11111111 */
		pad[0] = cc | 0x00;
		pad[1] = 0x00; pad[2] = 0x00; pad[3] = 0xff; pad[4] = 0xff;
		lpad = 5;
		break;
	case 2: /* xx010000 00000100 00000001 00000000 */
		pad[0] = cc | 0x08;
		pad[1] = 0x20;
		pad[2] = 0x80;
		pad[3] = 0x00;
		lpad = 4;
		break;
	case 4: /* xxxx0100 00000001 00000000 */
		pad[0] = cc | 0x20;
		pad[1] = 0x80;
		pad[2] = 0x00;
		lpad = 3;
		break;
	case 6: /* xxxxxx01 00000000 */
		pad[0] = cc | 0x80;
		pad[1] = 0x00;
		lpad = 2;
		break;
	case 7:	/* xxxxxxx0 00...... 00000000 00000000 11111111 11111111 */
		pad[0] = cc | 0x00;
		pad[1] = 0x00;
		pad[2] = 0x00; pad[3] = 0x00; pad[4] = 0xff; pad[5] = 0xff;
		lpad = 6;
		break;
	default:
		INCOMPL();
	}
	if (lpad > 0)
		ved_sendchunk(sp, NULL, 0, pad, lpad);
	st = VTAILQ_LAST(&sp->obj->store, storagehead);
	assert(st->len > 8);

	p = st->ptr + st->len - 8;
	icrc = vle32dec(p);
	ilen = vle32dec(p + 4);
//printf("CRC %08x LEN %d\n", icrc, ilen);
	sp->wrk->crc = crc32_combine(sp->wrk->crc, icrc, ilen);
	sp->wrk->l_crc += ilen;
}
