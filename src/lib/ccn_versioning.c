/*
 * ccn_versioning.c
 * Copyright (C) 2009 Palo Alto Research Center, Inc. All rights reserved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ccn/bloom.h>
#include <ccn/ccn.h>
#include <ccn/charbuf.h>
#include <ccn/uri.h>
#include <ccn/ccn_private.h>

#define FF 0xff

/*
 * This appends a tagged, valid, fully-saturated Bloom filter, useful for
 * excluding everything between two 'fenceposts' in an Exclude construct.
 */
static void
append_bf_all(struct ccn_charbuf *c)
{
    unsigned char bf_all[9] = { 3, 1, 'A', 0, 0, 0, 0, 0, 0xFF };
    const struct ccn_bloom_wire *b = ccn_bloom_validate_wire(bf_all, sizeof(bf_all));
    if (b == NULL) abort();
    ccn_charbuf_append_tt(c, CCN_DTAG_Bloom, CCN_DTAG);
    ccn_charbuf_append_tt(c, sizeof(bf_all), CCN_BLOB);
    ccn_charbuf_append(c, bf_all, sizeof(bf_all));
    ccn_charbuf_append_closer(c);
}

/*
 * Append AnswerOriginKind=1 to partially constructed Interest, meaning
 * do not generate new content.
 */
static void
answer_passive(struct ccn_charbuf *templ)
{
    ccn_charbuf_append_tt(templ, CCN_DTAG_AnswerOriginKind, CCN_DTAG);
    ccn_charbuf_append_tt(templ, 1, CCN_UDATA);
    ccn_charbuf_append(templ, "1", 1);
    ccn_charbuf_append_closer(templ); /* </AnswerOriginKind> */
}

/*
 * Append OrderPreference=5 to partially constructed Interest, meaning
 * prefer to send bigger.
 */
static void
answer_highest(struct ccn_charbuf *templ)
{
    ccn_charbuf_append_tt(templ, CCN_DTAG_OrderPreference, CCN_DTAG);
    ccn_charbuf_append_tt(templ, 1, CCN_UDATA);
    ccn_charbuf_append(templ, "5", 1);
    ccn_charbuf_append_closer(templ); /* </OrderPreference> */
}

static void
append_future_vcomp(struct ccn_charbuf *templ)
{
    /* A distant future version stamp */
    unsigned char b[7] = {CCN_MARKER_VERSION, FF, FF, FF, FF, FF, FF};
    ccn_charbuf_append_tt(templ, CCN_DTAG_Component, CCN_DTAG);
    ccn_charbuf_append_tt(templ, sizeof(b), CCN_BLOB);
    ccn_charbuf_append(templ, b, sizeof(b));
    ccn_charbuf_append_closer(templ); /* </Component> */
}

static struct ccn_charbuf *
resolve_templ(struct ccn_charbuf *templ, unsigned const char *vcomp, int size)
{
    if (templ == NULL)
        templ = ccn_charbuf_create();
    if (size < 3 || size > 16) {
        ccn_charbuf_destroy(&templ);
        return(NULL);
    }
    templ->length = 0;
    ccn_charbuf_append_tt(templ, CCN_DTAG_Interest, CCN_DTAG);
    ccn_charbuf_append_tt(templ, CCN_DTAG_Name, CCN_DTAG);
    ccn_charbuf_append_closer(templ); /* </Name> */
    ccn_charbuf_append_tt(templ, CCN_DTAG_Exclude, CCN_DTAG);
    append_bf_all(templ);
    ccn_charbuf_append_tt(templ, CCN_DTAG_Component, CCN_DTAG);
    ccn_charbuf_append_tt(templ, size, CCN_BLOB);
    ccn_charbuf_append(templ, vcomp, size);
    ccn_charbuf_append_closer(templ); /* </Component> */
    append_future_vcomp(templ);
    append_bf_all(templ);
    ccn_charbuf_append_closer(templ); /* </Exclude> */
    answer_highest(templ);
    answer_passive(templ);
    ccn_charbuf_append_closer(templ); /* </Interest> */
    return(templ);
}

/**
 * Resolve the version, based on existing ccn content.
 * @param h is the the ccn handle; it may be NULL, but it is preferable to
 *        use the handle that the client probably already has.
 * @param name is a ccnb-encoded Name prefix. It gets extended in-place with
 *        one additional Component such that it names highest extant
 *        version that can be found, subject to the supplied timeout.
 * @param versioning_flags presently must be CCN_V_HIGHEST
 * @param timeout_ms is a time value in milliseconds. This is applied per
 *        fetch attempt, so the total time may be longer by a factor that
 *        depends on the number of (ccn) hops to the source(s).
 * @returns -1 for error, 0 if name could not be extended, 1 if was.
 */
int
ccn_resolve_version(struct ccn *h, struct ccn_charbuf *name,
                    int versioning_flags, int timeout_ms)
{
    int res;
    int myres = -1;
    struct ccn_parsed_ContentObject pco_space = { 0 };
    struct ccn_charbuf *templ = NULL;
    struct ccn_charbuf *result = ccn_charbuf_create();
    struct ccn_parsed_ContentObject *pco = &pco_space;
    struct ccn_indexbuf *ndx = ccn_indexbuf_create();
    const unsigned char *vers = NULL;
    size_t vers_size = 0;
    int n = ccn_name_split(name, NULL);
    struct ccn_indexbuf *nix = ccn_indexbuf_create();
    unsigned char lowtime[7] = {CCN_MARKER_VERSION, 0, FF, FF, FF, FF, FF};
    
    if (versioning_flags != CCN_V_HIGHEST)
        goto Finish;
    n = ccn_name_split(name, nix);
    if (n < 0)
        goto Finish;
    templ = resolve_templ(templ, lowtime, sizeof(lowtime));
    result->length = 0;
    res = ccn_get(h, name, -1, templ, timeout_ms, result, pco, ndx);
    while (result->length != 0) {
        if (pco->type == CCN_CONTENT_NACK) // XXX - also check for number of components
            break;
        res = ccn_name_comp_get(result->buf, ndx, n, &vers, &vers_size);
        if (res < 0)
            break;
        if (vers_size == 7 && vers[0] == CCN_MARKER_VERSION) {
            /* Looks like we have versions. */
            res = ccn_name_chop(name, nix, n);
            if (res != n) abort();
            ccn_name_append(name, vers, vers_size);
            ccn_name_split(name, nix); // XXX should have append that updates nix, too
            myres = 0;
            templ = resolve_templ(templ, name->buf + nix->buf[n], nix->buf[n+1] - nix->buf[n]);
            if (templ == NULL) break;
            result->length = 0;
            res = ccn_get(h, name, n, templ, timeout_ms, result, pco, ndx);
        }
        else break;
    }
Finish:
    ccn_charbuf_destroy(&result);
    ccn_indexbuf_destroy(&ndx);
    ccn_indexbuf_destroy(&nix);
    ccn_charbuf_destroy(&templ);
    return(myres);
}

/**
 * Extend a Name with a new version stamp
 * @param h is the the ccn handle.
 *        May be NULL.  This procedure does not use the connection.
 * @param name is a ccnb-encoded Name prefix. By default it gets extended in-place with
 *        one additional Component that conforms the the versioning profile
 *        and is based on the supplied time.
 * @param versioning_flags modifies the default behavior:
 *        CCN_V_REPLACE causes the last component to be replaced if it
 *        appears to be a version stamp.  If CCN_V_HIGH is set as well, an
 *        attempt will be made to generate a new version stamp that is
 *        later than the existing one, or to return an error.
 *        CCN_V_NOW bases the version on the current time rather than the
 *        supplied time.
 * @param secs is the desired time, in seconds since epoch (ignored if CCN_V_NOW is set).
 * @param nsecs is the number of nanoseconds
 * @returns -1 for error, 0 for success.
 */
int
ccn_create_version(struct ccn *h, struct ccn_charbuf *name,
                   int versioning_flags, intmax_t secs, int nsecs)
{
    struct ccn_indexbuf *nix = ccn_indexbuf_create();
    int n = ccn_name_split(name, nix);
    int myres = -1;
    size_t i;
    size_t j;
    size_t lc;
    size_t oc;
    // XXX - right now we ignore h, but in the future we may use it to try to avoid non-monotonicies in the versions.
    
    if (n < 0)
        goto Finish;
    if ((versioning_flags & ~(CCN_V_REPLACE | CCN_V_HIGH | CCN_V_NOW)) != 0)
        goto Finish;        
    name->length -= 1; /* Strip name closer */
    i = name->length;
    myres = 0;
    myres |= ccn_charbuf_append_tt(name, CCN_DTAG_Component, CCN_DTAG);
    if ((versioning_flags & CCN_V_NOW) != 0)
        myres |= ccn_charbuf_append_now_blob(name, CCN_MARKER_VERSION);
    else {
        myres |= ccn_charbuf_append_timestamp_blob(name, CCN_MARKER_VERSION, secs, nsecs);
    }
    myres |= ccn_charbuf_append_closer(name); /* </Component> */
    if (myres < 0) {
        name->length = i;
        goto CloseName;
    }
    j = name->length;
    if (n >= 1 && (versioning_flags & CCN_V_REPLACE) != 0) {
        oc = nix->buf[n-1];
        lc = nix->buf[n] - oc;
        if (lc <= 11 && lc >= 6 && name->buf[oc + 2] == CCN_MARKER_VERSION) {
            if ((versioning_flags & CCN_V_HIGH) != 0 && memcmp(name->buf + oc, name->buf + i, j - i) > 0) {
                /* Supplied version is in the future. */
                name->length = i;
                // XXX - we could try harder to make this work, for now just error out
                myres = -1;
                goto CloseName;
            }
            memmove(name->buf + oc, name->buf + i, j - i);
            name->length -= lc;
        }
    }
CloseName:
    myres |= ccn_charbuf_append_closer(name); /* </Name> */
Finish:
    myres = (myres < 0) ? -1 : 0;
    ccn_indexbuf_destroy(&nix);
    return(myres);
}
