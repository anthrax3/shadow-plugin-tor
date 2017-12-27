/*
 * The Shadow Simulator
 * See LICENSE for licensing information
 */

#include "torflow.h"

struct _TorFlowSlice {
    guint sliceID;
    gdouble percentile;
    guint numProbesPerRelay;
    guint totalProbesRemaining;

    GHashTable* entries;
    GHashTable* exits;
};

static void _torflowslice_computeMinProbes(gchar* key, gpointer value, guint* minProbes) {
    guint probes = GPOINTER_TO_UINT(value);
    if(*minProbes < probes) {
        *minProbes = probes;
    }
}

static GQueue* _torflowslice_getCandidates(TorFlowSlice* slice, GHashTable* table) {
    g_assert(slice);
    g_assert(table);

    /* first get the minimum number of probes that we have for any relay */
    guint minProbes = 0;
    g_hash_table_foreach(table, (GHFunc)_torflowslice_computeMinProbes, &minProbes);

    /* now collect all relays that have the same minimum value */
    GQueue* candidates = g_queue_new();

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, table);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        /* the key is the gchar* relay identity */
        g_queue_push_tail(candidates, key);
    }

    return candidates;
}

static guint _torflowslice_randomIndex(guint numElements) {
    gint rInt = rand();
    gdouble rDouble = (gdouble)(((gdouble)rInt) / ((gdouble)RAND_MAX));
    guint randomIndex = (guint)(rDouble * ((gdouble)(numElements-1)));
    return randomIndex >= numElements ? (numElements-1) : randomIndex;
}

static void _torflowslice_countProbesRemaining(gchar* key, gpointer value, TorFlowSlice* slice) {
    g_assert(slice);

    guint numProbes = GPOINTER_TO_UINT(value);
    guint remaining = numProbes >= slice->numProbesPerRelay ? 0 : slice->numProbesPerRelay - numProbes;

    slice->totalProbesRemaining += remaining;
}

TorFlowSlice* torflowslice_new(guint sliceID, gdouble percentile, guint numProbesPerRelay) {
    TorFlowSlice* slice = g_new0(TorFlowSlice, 1);

    slice->sliceID = sliceID;
    slice->percentile = percentile;
    slice->numProbesPerRelay = numProbesPerRelay;

    slice->entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    slice->exits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    return slice;
}

void torflowslice_free(TorFlowSlice* slice) {
    g_assert(slice);

    if(slice->entries) {
        g_hash_table_destroy(slice->entries);
    }

    if(slice->exits) {
        g_hash_table_destroy(slice->exits);
    }

    g_free(slice);
}

void torflowslice_addRelay(TorFlowSlice* slice, TorFlowRelay* relay) {
    g_assert(slice);
    g_assert(relay);

    gchar* relayID = g_strdup(torflowrelay_getIdentity(relay));
    guint numProbes = 0;

    if(torflowrelay_getIsExit(relay)) {
        g_hash_table_replace(slice->exits, relayID, GUINT_TO_POINTER(numProbes));
    } else {
        g_hash_table_replace(slice->entries, relayID, GUINT_TO_POINTER(numProbes));
    }
}

guint torflowslice_getLength(TorFlowSlice* slice) {
    g_assert(slice);
    return g_hash_table_size(slice->exits) + g_hash_table_size(slice->entries);
}

guint torflowslice_getNumProbesRemaining(TorFlowSlice* slice) {
    g_assert(slice);

    slice->totalProbesRemaining = 0;
    g_hash_table_foreach(slice->entries, (GHFunc)_torflowslice_countProbesRemaining, slice);
    g_hash_table_foreach(slice->exits, (GHFunc)_torflowslice_countProbesRemaining, slice);

    return slice->totalProbesRemaining;
}

gsize torflowslice_getTransferSize(TorFlowSlice* slice) {
    g_assert(slice);

    gsize transferSize = 0;

#ifdef SMALLFILES
    /* Have torflow download smaller files than the real Torflow does.
     * This improves actual running time but should have little effect on
     * simulated timings. */
    if (percentile < 0.25) {
        transferSize = 256*1024; /* 256 KiB */
    } else if (percentile < 0.5) {
        transferSize = 128*1024; /* 128 KiB */
    } else if (percentile < 0.75) {
        transferSize = 64*1024; /* 64 KiB */
    } else {
        transferSize = 32*1024; /* 32 KiB */
    }
#else
    /* This is based not on the spec, but on the file read by TorFlow,
     * NetworkScanners/BwAuthority/data/bwfiles. */
    if (slice->percentile < 0.01) {
        transferSize = 4*1024*1024; /* 4 MiB */
    } else if (slice->percentile < 0.07) {
        transferSize = 2*1024*1024; /* 2 MiB */
    } else if (slice->percentile < 0.23) {
        transferSize = 1024*1024; /* 1 MiB */
    } else if (slice->percentile < 0.53) {
        transferSize = 512*1024; /* 512 KiB */
    } else if (slice->percentile < 0.82) {
        transferSize = 256*1024; /* 256 KiB */
    } else if (slice->percentile < 0.95) {
        transferSize = 128*1024; /* 128 KiB */
    } else if (slice->percentile < 0.99) {
        transferSize = 64*1024; /* 64 KiB */
    } else {
        transferSize = 32*1024; /* 32 KiB */
    }
#endif

    return transferSize;
}

gboolean torflowslice_chooseRelayPair(TorFlowSlice* slice, gchar** entryRelayIdentity, gchar** exitRelayIdentity) {
    g_assert(slice);

    /* choose an entry and exit among entries and exits with the lowest measurement counts */
    GQueue* candidateEntries = _torflowslice_getCandidates(slice, slice->entries);
    GQueue* candidateExits = _torflowslice_getCandidates(slice, slice->exits);

    /* make sure we have at least one entry and one exit */
    if(g_queue_is_empty(candidateEntries) && g_queue_is_empty(candidateExits)) {
        warning("problem choosing relay pair: found %u candidates of %u entries and %u candidates of %u exits",
                g_queue_get_length(candidateEntries), g_hash_table_size(slice->entries),
                g_queue_get_length(candidateExits), g_hash_table_size(slice->exits));

        g_queue_free(candidateEntries);
        g_queue_free(candidateExits);

        return FALSE;
    }

    /* choose uniformly from all candidates */
    guint entryPosition = _torflowslice_randomIndex(g_queue_get_length(candidateEntries));
    guint exitPosition = _torflowslice_randomIndex(g_queue_get_length(candidateExits));

    gchar* entryID = g_queue_peek_nth(candidateEntries, entryPosition);
    gchar* exitID = g_queue_peek_nth(candidateExits, exitPosition);

    if(entryID == NULL || exitID == NULL) {
        error("we had candidate exits and entries, but found NULL ids: entry=%s exit=%s", entryID, exitID);

        g_queue_free(candidateEntries);
        g_queue_free(candidateExits);

        return FALSE;
    }

    /* update the measurement count for the chosen relays */
    gpointer entryProbeCount = g_hash_table_lookup(slice->entries, entryID);
    gpointer exitProbeCount = g_hash_table_lookup(slice->exits, exitID);

    /* steal the keys so we don't free them */
    g_hash_table_steal(slice->entries, entryID);
    g_hash_table_steal(slice->exits, exitID);

    /* increment the measurement counts */
    guint newEntryCount = GPOINTER_TO_UINT(entryProbeCount);
    guint newExitCount = GPOINTER_TO_UINT(exitProbeCount);
    newEntryCount++;
    newExitCount++;

    /* store them in the table again */
    g_hash_table_replace(slice->entries, entryID, GUINT_TO_POINTER(newEntryCount));
    g_hash_table_replace(slice->exits, exitID, GUINT_TO_POINTER(newExitCount));

    info("choosing relay pair: found %u candidates of %u entries and %u candidates of %u exits, "
            "choosing entry %s at position %u and exit %s at position %u, "
            "new entry probe count is %u and exit probe count is %u",
            g_queue_get_length(candidateEntries), g_hash_table_size(slice->entries),
            g_queue_get_length(candidateExits), g_hash_table_size(slice->exits),
            entryID, entryPosition, exitID, exitPosition, newEntryCount, newExitCount);

    /* cleanup the queues */
    g_queue_free(candidateEntries);
    g_queue_free(candidateExits);

    /* return values */
    if(entryRelayIdentity) {
        *entryRelayIdentity = entryID;
    }
    if(exitRelayIdentity) {
        *exitRelayIdentity = exitID;
    }
    return TRUE;
}
