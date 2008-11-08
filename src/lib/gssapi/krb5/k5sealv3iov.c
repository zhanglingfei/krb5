/* -*- mode: c; indent-tabs-mode: nil -*- */
/*
 * lib/gssapi/krb5/k5sealv3iov.c
 *
 * Copyright 2008 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 *
 */

#include <assert.h>
#include "k5-platform.h"	/* for 64-bit support */
#include "k5-int.h"	     /* for zap() */
#include "gssapiP_krb5.h"
#include <stdarg.h>

krb5_error_code
gss_krb5int_make_seal_token_v3_iov(krb5_context context,
				   krb5_gss_ctx_id_rec *ctx,
				   int conf_req_flag,
				   int *conf_state,
				   size_t iov_count,
				   gss_iov_buffer_desc *iov,
				   int toktype)
{
    krb5_error_code code;
    gss_iov_buffer_t header;
    gss_iov_buffer_t padding;
    gss_iov_buffer_t trailer;
    unsigned char acceptor_flag;
    unsigned short tok_id;
    unsigned char *outbuf = NULL;
    int key_usage;
    size_t rrc, ec;
    size_t data_length, assoc_data_length;
    size_t gss_headerlen;
    krb5_keyblock *key;

    assert(toktype != KG_TOK_SEAL_MSG || ctx->enc != NULL);
    assert(ctx->big_endian == 0);

    acceptor_flag = ctx->initiate ? 0 : FLAG_SENDER_IS_ACCEPTOR;
    key_usage = (toktype == KG_TOK_WRAP_MSG
		 ? (ctx->initiate
		    ? KG_USAGE_INITIATOR_SEAL
		    : KG_USAGE_ACCEPTOR_SEAL)
		 : (ctx->initiate
		    ? KG_USAGE_INITIATOR_SIGN
		    : KG_USAGE_ACCEPTOR_SIGN));
    if (ctx->have_acceptor_subkey) {
	key = ctx->acceptor_subkey;
    } else {
	key = ctx->enc;
    }

    header = kg_locate_iov(iov_count, iov, GSS_IOV_BUFFER_TYPE_HEADER);
    if (header == NULL)
	return EINVAL;

    if (toktype == KG_TOK_WRAP_MSG && conf_req_flag) {
	padding = kg_locate_iov(iov_count, iov, GSS_IOV_BUFFER_TYPE_PADDING);
	if (padding == NULL && (ctx->gss_flags & GSS_C_DCE_STYLE) == 0)
	    return EINVAL;
    } else
	padding = NULL;

    trailer = kg_locate_iov(iov_count, iov, GSS_IOV_BUFFER_TYPE_TRAILER);

    kg_iov_msglen(iov_count, iov, &data_length, &assoc_data_length);

    outbuf = (unsigned char *)header->buffer.value;

    if (toktype == KG_TOK_WRAP_MSG && conf_req_flag) {
	size_t k5_headerlen, k5_padlen, k5_trailerlen;
	size_t gss_padlen = 0, gss_trailerlen = 0;

	code = krb5_c_crypto_length(context, key->enctype, KRB5_CRYPTO_TYPE_HEADER, &k5_headerlen);
	if (code != 0)
	    goto cleanup;

	code = krb5_c_crypto_length(context, key->enctype, KRB5_CRYPTO_TYPE_PADDING, &k5_padlen);
	if (code != 0)
	    goto cleanup;

	code = krb5_c_crypto_length(context, key->enctype, KRB5_CRYPTO_TYPE_TRAILER, &k5_trailerlen);
	if (code != 0)
	    goto cleanup;

	gss_headerlen = 16 /* Header */ + k5_headerlen;
	if (trailer == NULL)
	    gss_headerlen += 16 /* E(Header) */ + k5_trailerlen;
        else
	    gss_trailerlen = 16 /* E(Header) */ + k5_trailerlen;

	if (header->flags & GSS_IOV_BUFFER_FLAG_ALLOCATE)
	    code = kg_allocate_iov(header, gss_headerlen);
	else if (header->buffer.length < gss_headerlen)
	    code = KRB5_BAD_MSIZE;
	if (code != 0)
	    goto cleanup;

	if (k5_padlen != 0) {
	    /* DCE guarantees the data to be padded, padding buffer is optional */
	    size_t conf_data_length = 16 /* Header */ + data_length - assoc_data_length;

	    if (ctx->gss_flags & GSS_C_DCE_STYLE) {
		if ((conf_data_length + (padding != NULL ? padding->buffer.length : 0) % k5_padlen) != 0)
		    code = KRB5_BAD_MSIZE;
		else
		    gss_padlen = 0;
	    } else if (padding->flags & GSS_IOV_BUFFER_FLAG_ALLOCATE) {
		gss_padlen = k5_padlen - (conf_data_length % k5_padlen);
		code = kg_allocate_iov(padding, gss_padlen);
	    } else {
		gss_padlen = padding->buffer.length;
	    }
	    if (code != 0)
		goto cleanup;
	}

	if (padding != NULL)
	    memset(padding->buffer.value, 'x', padding->buffer.length);

	if (trailer->flags & GSS_IOV_BUFFER_FLAG_ALLOCATE)
	    code = kg_allocate_iov(trailer, gss_trailerlen);
	else if (trailer->buffer.length < gss_trailerlen)
	    code = KRB5_BAD_MSIZE;
	if (code != 0)
	    goto cleanup;

	/*
	 * Windows has a bug where it rotates by EC + RRC instead of RRC as
	 * specified in the RFC. The simplest workaround is to always send
	 * EC == 0, which means that Windows will rotate by the correct
	 * amount.
	 */
	ec = (ctx->gss_flags & GSS_C_DCE_STYLE) ? 0 : gss_padlen;

	if (trailer == NULL)
	    rrc = gss_trailerlen;
	else
	    rrc = 0;

	/* TOK_ID */
	store_16_be(0x0504, outbuf);
	/* flags */
	outbuf[2] = (acceptor_flag
		     | (conf_req_flag ? FLAG_WRAP_CONFIDENTIAL : 0)
		     | (ctx->have_acceptor_subkey ? FLAG_ACCEPTOR_SUBKEY : 0));
	/* filler */
	outbuf[3] = 0xFF;
	/* EC */
	store_16_be(ec, outbuf + 4);
	/* RRC */
	store_16_be(rrc, outbuf + 6);
	store_64_be(ctx->seq_send, outbuf + 8);

	/* Copy of header to be encrypted */
	if (trailer == NULL)
	    memcpy((unsigned char *)header->buffer.value + 16, header->buffer.value, 16);
	else
	    memcpy((unsigned char *)trailer->buffer.value, header->buffer.value, 16);

	code = kg_encrypt_iov(context, ctx->proto,
			      ((ctx->gss_flags & GSS_C_DCE_STYLE) != 0),
			      ec, rrc, key, key_usage, 0, iov_count, iov);
	if (code != 0)
	    goto cleanup;

	ctx->seq_send++;
    } else if (toktype == KG_TOK_WRAP_MSG && !conf_req_flag) {
	assert(ctx->cksum_size <= 0xFFFF);

	tok_id = 0x0504;

    wrap_with_checksum:

	if (trailer == NULL)
	    rrc = ctx->cksum_size;
	else
	    rrc = 0;

	/* TOK_ID */
	store_16_be(tok_id, outbuf);
	/* flags */
	outbuf[2] = (acceptor_flag
		     | (ctx->have_acceptor_subkey ? FLAG_ACCEPTOR_SUBKEY : 0));
	/* filler */
	outbuf[3] = 0xFF;
	if (toktype == KG_TOK_WRAP_MSG) {
	    /* Use 0 for checksum calculation, substitute
	     * checksum length later.
	     */
	    /* EC */
	    store_16_be(0, outbuf + 4);
	    /* RRC */
	    store_16_be(0, outbuf + 6);
	} else {
	    /* MIC and DEL store 0xFF in EC and RRC */
	    store_16_be(0xFFFF, outbuf + 4);
	    store_16_be(0xFFFF, outbuf + 6);
	}
	store_64_be(ctx->seq_send, outbuf + 8);

	code = kg_make_checksum_iov_v3(context, ctx->cksumtype,
				       rrc, key, key_usage,
				       iov_count, iov);
	if (code != 0)
	    goto cleanup;

	ctx->seq_send++;

	if (toktype == KG_TOK_WRAP_MSG) {
	    /* Fix up EC field */
	    store_16_be(ctx->cksum_size, outbuf + 4);
	    /* Fix up RRC field */
	    store_16_be(rrc, outbuf + 6);
	}
    } else if (toktype == KG_TOK_MIC_MSG) {
	tok_id = 0x0404;
	goto wrap_with_checksum;
    } else if (toktype == KG_TOK_DEL_CTX) {
	tok_id = 0x0405;
	goto wrap_with_checksum;
    } else {
	abort();
    }

    code = 0;

cleanup:
    kg_release_iov(iov_count, iov);

    return code;
}

OM_uint32
gss_krb5int_unseal_v3_iov(krb5_context context,
			  OM_uint32 *minor_status,
			  krb5_gss_ctx_id_rec *ctx,
			  size_t iov_count,
			  gss_iov_buffer_desc *iov,
			  int *conf_state,
			  gss_qop_t *qop_state,
			  int toktype)
{
    OM_uint32 code;
    gss_iov_buffer_t header;
    gss_iov_buffer_t trailer;
    unsigned char acceptor_flag;
    unsigned char *ptr = NULL;
    int key_usage;
    size_t rrc, ec;
    size_t data_length, assoc_data_length;
    krb5_keyblock *key;
    gssint_uint64 seqnum;
    krb5_boolean valid;

    assert(toktype != KG_TOK_SEAL_MSG || ctx->enc != 0);
    assert(ctx->big_endian == 0);
    assert(ctx->proto == 1);

    if (qop_state != NULL)
	*qop_state = GSS_C_QOP_DEFAULT;

    header = kg_locate_iov(iov_count, iov, GSS_IOV_BUFFER_TYPE_HEADER);
    assert(header != NULL);

    trailer = kg_locate_iov(iov_count, iov, GSS_IOV_BUFFER_TYPE_TRAILER);

    acceptor_flag = ctx->initiate ? 0 : FLAG_SENDER_IS_ACCEPTOR;
    key_usage = (toktype == KG_TOK_WRAP_MSG
		 ? (ctx->initiate
		    ? KG_USAGE_INITIATOR_SEAL
		    : KG_USAGE_ACCEPTOR_SEAL)
		 : (ctx->initiate
		    ? KG_USAGE_INITIATOR_SIGN
		    : KG_USAGE_ACCEPTOR_SIGN));

    kg_iov_msglen(iov_count, iov, &data_length, &assoc_data_length);

    ptr = (unsigned char *)header->buffer.value;

    if (header->buffer.length < 16) {
	*minor_status = 0;
	return GSS_S_DEFECTIVE_TOKEN;
    }

    if ((ptr[2] & FLAG_SENDER_IS_ACCEPTOR) != acceptor_flag) {
	*minor_status = G_BAD_DIRECTION;
	return GSS_S_BAD_SIG;
    }

    if (ctx->have_acceptor_subkey && (ptr[2] & FLAG_ACCEPTOR_SUBKEY)) {
	key = ctx->acceptor_subkey;
    } else {
	key = ctx->enc;
    }

    if (toktype == KG_TOK_WRAP_MSG) {
	if (load_16_be(ptr) != 0x0505)
	    goto defective;
	if (ptr[3] != 0xFF)
	    goto defective;
	ec = load_16_be(ptr + 4);
	rrc = load_16_be(ptr + 6);
	seqnum = load_64_be(ptr + 8);

	/* Deal with RRC */
	if (trailer == NULL) {
	    /* According to MS, we only need to deal with a fixed RRC for DCE */
	    if (rrc != (ptr[2] & FLAG_WRAP_CONFIDENTIAL) ? 16 + ctx->cksum_size : ctx->cksum_size)
		goto defective;
	} else if (rrc != 0) {
	    /* Should have been rotated by kg_tokenize_stream_iov() */
	    goto defective;
	}

	if (ptr[2] & FLAG_WRAP_CONFIDENTIAL) {
	    unsigned char *althdr;

	    /* Decrypt */
	    code = kg_decrypt_iov(context, ctx->proto,
				  ((ctx->gss_flags & GSS_C_DCE_STYLE) != 0),
				  ec, rrc,
				  key, key_usage, 0, iov_count, iov);
	    if (code != 0) {
		*minor_status = code;
		return GSS_S_BAD_SIG;
	    }

	    /* Validate header integrity */
	    althdr = (unsigned char *)header->buffer.value;

	    if (load_16_be(althdr) != 0x0504
		|| althdr[2] != ptr[2]
		|| althdr[3] != ptr[3]
		|| memcmp(althdr + 8, ptr + 8, 8) != 0) {
		*minor_status = 0;
		return GSS_S_BAD_SIG;
	    }

	    /* caller should have fixed up padding */
	} else {
	    /* Verify checksum: note EC is checksum size here, not padding */
	    if (ec != ctx->cksum_size)
		goto defective;

	    /* Zero EC, RRC before computing checksum */
	    store_16_be(0, ptr + 4);
	    store_16_be(0, ptr + 6);

	    code = kg_verify_checksum_iov_v3(context, ctx->cksumtype, rrc,
					     key, key_usage, iov_count, iov, &valid);
	    if (code != 0 || valid == 0) {
		*minor_status = code;
		return GSS_S_BAD_SIG;
	    }
	}

	code = g_order_check(&ctx->seqstate, seqnum);
    } else if (toktype == KG_TOK_MIC_MSG) {
	if (load_16_be(ptr) != 0x0404)
	    goto defective;

    verify_mic_1:
	if (ptr[3] != 0xFF)
	    goto defective;
	seqnum = load_64_be(ptr + 8);

	code = kg_verify_checksum_iov_v3(context, ctx->cksumtype, 0,
					 key, key_usage, iov_count, iov, &valid);
	if (code != 0 || valid == 0) {
	    *minor_status = code;
	    return GSS_S_BAD_SIG;
	}
	code = g_order_check(&ctx->seqstate, seqnum);
    } else if (toktype == KG_TOK_DEL_CTX) {
	if (load_16_be(ptr) != 0x0405)
	    goto defective;
	goto verify_mic_1;
    } else {
	goto defective;
    }

    *minor_status = 0;

    if (conf_state != NULL)
	*conf_state = ((ptr[2] & FLAG_WRAP_CONFIDENTIAL) != 0);

    return code;

defective:
    *minor_status = 0;

    return GSS_S_DEFECTIVE_TOKEN;
}
