/*
   IGraph library.
   Copyright (C) 2003-2024  The igraph development team <igraph@igraph.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "igraph_games.h"

#include "igraph_adjlist.h"
#include "igraph_bitset_list.h"
#include "igraph_constructors.h"
#include "igraph_conversion.h"
#include "igraph_graphicality.h"
#include "igraph_interface.h"
#include "igraph_memory.h"
#include "igraph_operators.h"
#include "igraph_random.h"
#include "igraph_vector_ptr.h"

#include "core/interruption.h"
#include "core/set.h"
#include "games/degree_sequence_vl/degree_sequence_vl.h"
#include "math/safe_intop.h"

static igraph_error_t configuration(
        igraph_t *graph,
        const igraph_vector_int_t *out_seq,
        const igraph_vector_int_t *in_seq) {

    const igraph_bool_t directed = (in_seq != NULL && igraph_vector_int_size(in_seq) != 0);
    igraph_integer_t outsum = 0, insum = 0;
    igraph_bool_t graphical;
    igraph_integer_t no_of_nodes, no_of_edges;
    igraph_integer_t *bag1, *bag2;
    igraph_integer_t bagp1 = 0, bagp2 = 0;
    igraph_vector_int_t edges;

    IGRAPH_CHECK(igraph_is_graphical(out_seq, in_seq, IGRAPH_LOOPS_SW | IGRAPH_MULTI_SW, &graphical));
    if (!graphical) {
        IGRAPH_ERROR(in_seq ? "No directed graph can realize the given degree sequences." :
                     "No undirected graph can realize the given degree sequence.", IGRAPH_EINVAL);
    }

    IGRAPH_CHECK(igraph_i_safe_vector_int_sum(out_seq, &outsum));
    if (directed) {
        IGRAPH_CHECK(igraph_i_safe_vector_int_sum(in_seq, &insum));
    }

    no_of_nodes = igraph_vector_int_size(out_seq);
    no_of_edges = directed ? outsum : outsum / 2;

    bag1 = IGRAPH_CALLOC(outsum, igraph_integer_t);
    IGRAPH_CHECK_OOM(bag1, "Insufficient memory for sampling from configuration model.");
    IGRAPH_FINALLY(igraph_free, bag1);

    for (igraph_integer_t i = 0; i < no_of_nodes; i++) {
        for (igraph_integer_t j = 0; j < VECTOR(*out_seq)[i]; j++) {
            bag1[bagp1++] = i;
        }
    }
    if (directed) {
        bag2 = IGRAPH_CALLOC(insum, igraph_integer_t);
        IGRAPH_CHECK_OOM(bag2, "Insufficient memory for sampling from configuration model.");
        IGRAPH_FINALLY(igraph_free, bag2);
        for (igraph_integer_t i = 0; i < no_of_nodes; i++) {
            for (igraph_integer_t j = 0; j < VECTOR(*in_seq)[i]; j++) {
                bag2[bagp2++] = i;
            }
        }
    }

    IGRAPH_VECTOR_INT_INIT_FINALLY(&edges, 0);
    IGRAPH_CHECK(igraph_vector_int_reserve(&edges, no_of_edges * 2));

    RNG_BEGIN();

    if (directed) {
        for (igraph_integer_t i = 0; i < no_of_edges; i++) {
            igraph_integer_t from = RNG_INTEGER(0, bagp1 - 1);
            igraph_integer_t to = RNG_INTEGER(0, bagp2 - 1);
            igraph_vector_int_push_back(&edges, bag1[from]); /* safe, already reserved */
            igraph_vector_int_push_back(&edges, bag2[to]);   /* ditto */
            bag1[from] = bag1[bagp1 - 1];
            bag2[to] = bag2[bagp2 - 1];
            bagp1--; bagp2--;
        }
    } else {
        for (igraph_integer_t i = 0; i < no_of_edges; i++) {
            igraph_integer_t from = RNG_INTEGER(0, bagp1 - 1);
            igraph_integer_t to;
            igraph_vector_int_push_back(&edges, bag1[from]); /* safe, already reserved */
            bag1[from] = bag1[bagp1 - 1];
            bagp1--;
            to = RNG_INTEGER(0, bagp1 - 1);
            igraph_vector_int_push_back(&edges, bag1[to]);   /* ditto */
            bag1[to] = bag1[bagp1 - 1];
            bagp1--;
        }
    }

    RNG_END();

    IGRAPH_FREE(bag1);
    IGRAPH_FINALLY_CLEAN(1);
    if (directed) {
        IGRAPH_FREE(bag2);
        IGRAPH_FINALLY_CLEAN(1);
    }

    IGRAPH_CHECK(igraph_create(graph, &edges, no_of_nodes, directed));
    igraph_vector_int_destroy(&edges);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}

static igraph_error_t fast_heur_undirected(
        igraph_t *graph,
        const igraph_vector_int_t *seq) {

    igraph_vector_int_t stubs;
    igraph_vector_int_t *neis;
    igraph_vector_int_t residual_degrees;
    igraph_set_t incomplete_vertices;
    igraph_adjlist_t al;
    igraph_bool_t finished, failed;
    igraph_integer_t from, to, dummy;
    igraph_integer_t i, j, k;
    igraph_integer_t no_of_nodes, outsum = 0;
    igraph_bool_t graphical;
    int iter = 0;

    IGRAPH_CHECK(igraph_is_graphical(seq, 0, IGRAPH_SIMPLE_SW, &graphical));
    if (!graphical) {
        IGRAPH_ERROR("No simple undirected graph can realize the given degree sequence.",
                     IGRAPH_EINVAL);
    }

    IGRAPH_CHECK(igraph_i_safe_vector_int_sum(seq, &outsum));
    no_of_nodes = igraph_vector_int_size(seq);

    /* Allocate required data structures */
    IGRAPH_CHECK(igraph_adjlist_init_empty(&al, no_of_nodes));
    IGRAPH_FINALLY(igraph_adjlist_destroy, &al);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&stubs, 0);
    IGRAPH_CHECK(igraph_vector_int_reserve(&stubs, outsum));
    IGRAPH_VECTOR_INT_INIT_FINALLY(&residual_degrees, no_of_nodes);
    IGRAPH_CHECK(igraph_set_init(&incomplete_vertices, 0));
    IGRAPH_FINALLY(igraph_set_destroy, &incomplete_vertices);

    /* Start the RNG */
    RNG_BEGIN();

    /* Outer loop; this will try to construct a graph several times from scratch
     * until it finally succeeds. */
    finished = false;
    while (!finished) {
        IGRAPH_ALLOW_INTERRUPTION_LIMITED(iter, 1 << 8);

        /* Be optimistic :) */
        failed = false;

        /* Clear the adjacency list to get rid of the previous attempt (if any) */
        igraph_adjlist_clear(&al);

        /* Initialize the residual degrees from the degree sequence */
        IGRAPH_CHECK(igraph_vector_int_update(&residual_degrees, seq));

        /* While there are some unconnected stubs left... */
        while (!finished && !failed) {
            /* Construct the initial stub vector */
            igraph_vector_int_clear(&stubs);
            for (i = 0; i < no_of_nodes; i++) {
                for (j = 0; j < VECTOR(residual_degrees)[i]; j++) {
                    igraph_vector_int_push_back(&stubs, i);
                }
            }

            /* Clear the skipped stub counters and the set of incomplete vertices */
            igraph_vector_int_null(&residual_degrees);
            igraph_set_clear(&incomplete_vertices);

            /* Shuffle the stubs in-place */
            igraph_vector_int_shuffle(&stubs);

            /* Connect the stubs where possible */
            k = igraph_vector_int_size(&stubs);
            for (i = 0; i < k; ) {
                from = VECTOR(stubs)[i++];
                to = VECTOR(stubs)[i++];

                if (from > to) {
                    dummy = from; from = to; to = dummy;
                }

                neis = igraph_adjlist_get(&al, from);
                if (from == to || igraph_vector_int_binsearch(neis, to, &j)) {
                    /* Edge exists already */
                    VECTOR(residual_degrees)[from]++;
                    VECTOR(residual_degrees)[to]++;
                    IGRAPH_CHECK(igraph_set_add(&incomplete_vertices, from));
                    IGRAPH_CHECK(igraph_set_add(&incomplete_vertices, to));
                } else {
                    /* Insert the edge */
                    IGRAPH_CHECK(igraph_vector_int_insert(neis, j, to));
                }
            }

            finished = igraph_set_empty(&incomplete_vertices);

            if (!finished) {
                /* We are not done yet; check if the remaining stubs are feasible. This
                 * is done by enumerating all possible pairs and checking whether at
                 * least one feasible pair is found. */
                i = 0;
                failed = true;
                while (failed && igraph_set_iterate(&incomplete_vertices, &i, &from)) {
                    j = 0;
                    while (igraph_set_iterate(&incomplete_vertices, &j, &to)) {
                        if (from == to) {
                            /* This is used to ensure that each pair is checked once only */
                            break;
                        }
                        if (from > to) {
                            dummy = from; from = to; to = dummy;
                        }
                        neis = igraph_adjlist_get(&al, from);
                        if (!igraph_vector_int_binsearch(neis, to, 0)) {
                            /* Found a suitable pair, so we can continue */
                            failed = false;
                            break;
                        }
                    }
                }
            }
        }
    }

    /* Finish the RNG */
    RNG_END();

    /* Clean up */
    igraph_set_destroy(&incomplete_vertices);
    igraph_vector_int_destroy(&residual_degrees);
    igraph_vector_int_destroy(&stubs);
    IGRAPH_FINALLY_CLEAN(3);

    /* Create the graph. We cannot use IGRAPH_ALL here for undirected graphs
     * because we did not add edges in both directions in the adjacency list.
     * We will use igraph_to_undirected in an extra step. */
    IGRAPH_CHECK(igraph_adjlist(graph, &al, IGRAPH_OUT, 1));
    IGRAPH_CHECK(igraph_to_undirected(graph, IGRAPH_TO_UNDIRECTED_EACH, 0));

    /* Clear the adjacency list */
    igraph_adjlist_destroy(&al);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}

static igraph_error_t fast_heur_directed(
        igraph_t *graph,
        const igraph_vector_int_t *out_seq,
        const igraph_vector_int_t *in_seq) {

    igraph_adjlist_t al;
    igraph_bool_t deg_seq_ok, failed, finished;
    igraph_vector_int_t in_stubs;
    igraph_vector_int_t out_stubs;
    igraph_vector_int_t *neis;
    igraph_vector_int_t residual_in_degrees, residual_out_degrees;
    igraph_set_t incomplete_in_vertices, incomplete_out_vertices;
    igraph_integer_t from, to;
    igraph_integer_t i, j, k;
    igraph_integer_t no_of_nodes, outsum;
    int iter = 0;

    IGRAPH_CHECK(igraph_is_graphical(out_seq, in_seq, IGRAPH_SIMPLE_SW, &deg_seq_ok));
    if (!deg_seq_ok) {
        IGRAPH_ERROR("No simple directed graph can realize the given degree sequence.",
                     IGRAPH_EINVAL);
    }

    IGRAPH_CHECK(igraph_i_safe_vector_int_sum(out_seq, &outsum));
    no_of_nodes = igraph_vector_int_size(out_seq);

    /* Allocate required data structures */
    IGRAPH_CHECK(igraph_adjlist_init_empty(&al, no_of_nodes));
    IGRAPH_FINALLY(igraph_adjlist_destroy, &al);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&out_stubs, 0);
    IGRAPH_CHECK(igraph_vector_int_reserve(&out_stubs, outsum));
    IGRAPH_VECTOR_INT_INIT_FINALLY(&in_stubs, 0);
    IGRAPH_CHECK(igraph_vector_int_reserve(&in_stubs, outsum));
    IGRAPH_VECTOR_INT_INIT_FINALLY(&residual_out_degrees, no_of_nodes);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&residual_in_degrees, no_of_nodes);
    IGRAPH_CHECK(igraph_set_init(&incomplete_out_vertices, 0));
    IGRAPH_FINALLY(igraph_set_destroy, &incomplete_out_vertices);
    IGRAPH_CHECK(igraph_set_init(&incomplete_in_vertices, 0));
    IGRAPH_FINALLY(igraph_set_destroy, &incomplete_in_vertices);

    /* Start the RNG */
    RNG_BEGIN();

    /* Outer loop; this will try to construct a graph several times from scratch
     * until it finally succeeds. */
    finished = false;
    while (!finished) {
        IGRAPH_ALLOW_INTERRUPTION_LIMITED(iter, 1 << 8);

        /* Be optimistic :) */
        failed = false;

        /* Clear the adjacency list to get rid of the previous attempt (if any) */
        igraph_adjlist_clear(&al);

        /* Initialize the residual degrees from the degree sequences */
        IGRAPH_CHECK(igraph_vector_int_update(&residual_out_degrees, out_seq));
        IGRAPH_CHECK(igraph_vector_int_update(&residual_in_degrees, in_seq));

        /* While there are some unconnected stubs left... */
        while (!finished && !failed) {
            /* Construct the initial stub vectors */
            igraph_vector_int_clear(&out_stubs);
            igraph_vector_int_clear(&in_stubs);
            for (i = 0; i < no_of_nodes; i++) {
                for (j = 0; j < VECTOR(residual_out_degrees)[i]; j++) {
                    igraph_vector_int_push_back(&out_stubs, i);
                }
                for (j = 0; j < VECTOR(residual_in_degrees)[i]; j++) {
                    igraph_vector_int_push_back(&in_stubs, i);
                }
            }

            /* Clear the skipped stub counters and the set of incomplete vertices */
            igraph_vector_int_null(&residual_out_degrees);
            igraph_vector_int_null(&residual_in_degrees);
            igraph_set_clear(&incomplete_out_vertices);
            igraph_set_clear(&incomplete_in_vertices);

            /* Shuffle the out-stubs in-place */
            igraph_vector_int_shuffle(&out_stubs);

            /* Connect the stubs where possible */
            k = igraph_vector_int_size(&out_stubs);
            for (i = 0; i < k; i++) {
                from = VECTOR(out_stubs)[i];
                to = VECTOR(in_stubs)[i];

                neis = igraph_adjlist_get(&al, from);
                if (from == to || igraph_vector_int_binsearch(neis, to, &j)) {
                    /* Edge exists already */
                    VECTOR(residual_out_degrees)[from]++;
                    VECTOR(residual_in_degrees)[to]++;
                    IGRAPH_CHECK(igraph_set_add(&incomplete_out_vertices, from));
                    IGRAPH_CHECK(igraph_set_add(&incomplete_in_vertices, to));
                } else {
                    /* Insert the edge */
                    IGRAPH_CHECK(igraph_vector_int_insert(neis, j, to));
                }
            }

            /* Are we finished? */
            finished = igraph_set_empty(&incomplete_out_vertices);

            if (!finished) {
                /* We are not done yet; check if the remaining stubs are feasible. This
                 * is done by enumerating all possible pairs and checking whether at
                 * least one feasible pair is found. */
                i = 0;
                failed = true;
                while (failed && igraph_set_iterate(&incomplete_out_vertices, &i, &from)) {
                    j = 0;
                    while (igraph_set_iterate(&incomplete_in_vertices, &j, &to)) {
                        neis = igraph_adjlist_get(&al, from);
                        if (from != to && !igraph_vector_int_binsearch(neis, to, 0)) {
                            /* Found a suitable pair, so we can continue */
                            failed = false;
                            break;
                        }
                    }
                }
            }
        }
    }

    /* Finish the RNG */
    RNG_END();

    /* Clean up */
    igraph_set_destroy(&incomplete_in_vertices);
    igraph_set_destroy(&incomplete_out_vertices);
    igraph_vector_int_destroy(&residual_in_degrees);
    igraph_vector_int_destroy(&residual_out_degrees);
    igraph_vector_int_destroy(&in_stubs);
    igraph_vector_int_destroy(&out_stubs);
    IGRAPH_FINALLY_CLEAN(6);

    /* Create the graph */
    IGRAPH_CHECK(igraph_adjlist(graph, &al, IGRAPH_OUT, true));

    /* Clear the adjacency list */
    igraph_adjlist_destroy(&al);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}

/* swap two elements of a vector_int */
#define SWAP_INT_ELEM(vec, i, j) \
    { \
        igraph_integer_t temp; \
        temp = VECTOR(vec)[i]; \
        VECTOR(vec)[i] = VECTOR(vec)[j]; \
        VECTOR(vec)[j] = temp; \
    }

/* Uses a set to check for multi-edges. Efficient for larger graphs, frugal with memory. */
static igraph_error_t configuration_simple_undirected_set(
        const igraph_vector_int_t *degseq,
        igraph_vector_int_t *stubs,
        igraph_integer_t vcount, igraph_integer_t stub_count) {

    const igraph_integer_t ecount = stub_count / 2;
    igraph_vector_ptr_t adjlist;
    int iter = 0;

    /* Build an adjacency list in terms of sets; used to check for multi-edges. */
    IGRAPH_CHECK(igraph_vector_ptr_init(&adjlist, vcount));
    IGRAPH_VECTOR_PTR_SET_ITEM_DESTRUCTOR(&adjlist, igraph_set_destroy);
    IGRAPH_FINALLY(igraph_vector_ptr_destroy_all, &adjlist);
    for (igraph_integer_t i = 0; i < vcount; ++i) {
        igraph_set_t *set = IGRAPH_CALLOC(1, igraph_set_t);
        IGRAPH_CHECK_OOM(set, "Insufficient memory for configuration model (simple graphs).");
        IGRAPH_CHECK(igraph_set_init(set, 0));
        VECTOR(adjlist)[i] = set;
        IGRAPH_CHECK(igraph_set_reserve(set, VECTOR(*degseq)[i]));
    }

    RNG_BEGIN();

    for (;;) {
        igraph_bool_t success = true;

        /* Shuffle stubs vector with Fisher-Yates and check for self-loops and multi-edges as we go. */
        for (igraph_integer_t i = 0; i < ecount; ++i) {
            igraph_integer_t k, from, to;

            k = RNG_INTEGER(2*i, stub_count-1);
            SWAP_INT_ELEM(*stubs, 2*i, k);

            k = RNG_INTEGER(2*i+1, stub_count-1);
            SWAP_INT_ELEM(*stubs, 2*i+1, k);

            from = VECTOR(*stubs)[2*i];
            to   = VECTOR(*stubs)[2*i+1];

            /* self-loop, fail */
            if (from == to) {
                success = false;
                break;
            }

            /* multi-edge, fail */
            if (igraph_set_contains((igraph_set_t *) VECTOR(adjlist)[to], from)) {
                success = false;
                break;
            }

            /* sets are already reserved */
            igraph_set_add((igraph_set_t *) VECTOR(adjlist)[to], from);
            igraph_set_add((igraph_set_t *) VECTOR(adjlist)[from], to);
        }

        if (success) {
            break;
        }

        /* Clear adjacency list. */
        for (igraph_integer_t j = 0; j < vcount; ++j) {
            igraph_set_clear((igraph_set_t *) VECTOR(adjlist)[j]);
        }

        IGRAPH_ALLOW_INTERRUPTION_LIMITED(iter, 1 << 8);
    }

    RNG_END();

    igraph_vector_ptr_destroy_all(&adjlist);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}

/* Uses a bitset to check for multi-edges. Efficient for smaller graphs. */
static igraph_error_t configuration_simple_undirected_bitset(
        igraph_vector_int_t *stubs,
        igraph_integer_t vcount, igraph_integer_t stub_count) {

    const igraph_integer_t ecount = stub_count / 2;
    igraph_bitset_list_t adjlist;
    int iter = 0;

    /* Build an adjacency list in terms of bitsets; used to check for multi-edges. */
    IGRAPH_BITSET_LIST_INIT_FINALLY(&adjlist, vcount);
    for (igraph_integer_t i = 0; i < vcount; ++i) {
        IGRAPH_CHECK(igraph_bitset_resize(igraph_bitset_list_get_ptr(&adjlist, i), vcount));
    }

    RNG_BEGIN();

    for (;;) {
        igraph_bool_t success = true;

        /* Shuffle stubs vector with Fisher-Yates and check for self-loops and multi-edges as we go. */
        for (igraph_integer_t i = 0; i < ecount; ++i) {
            igraph_integer_t k, from, to;

            k = RNG_INTEGER(2*i, stub_count-1);
            SWAP_INT_ELEM(*stubs, 2*i, k);

            k = RNG_INTEGER(2*i+1, stub_count-1);
            SWAP_INT_ELEM(*stubs, 2*i+1, k);

            from = VECTOR(*stubs)[2*i];
            to   = VECTOR(*stubs)[2*i+1];

            /* self-loop, fail */
            if (from == to) {
                success = false;
                break;
            }

            /* multi-edge, fail */
            if (IGRAPH_BIT_TEST(*igraph_bitset_list_get_ptr(&adjlist, to), from)) {
                success = false;
                break;
            }

            /* sets are already reserved */
            IGRAPH_BIT_SET(*igraph_bitset_list_get_ptr(&adjlist, to), from);
            IGRAPH_BIT_SET(*igraph_bitset_list_get_ptr(&adjlist, from), to);
        }

        if (success) {
            break;
        }

        /* Clear adjacency list. */
        for (igraph_integer_t j = 0; j < vcount; ++j) {
            igraph_bitset_null(igraph_bitset_list_get_ptr(&adjlist, j));
        }

        IGRAPH_ALLOW_INTERRUPTION_LIMITED(iter, 1 << 8);
    }

    RNG_END();

    igraph_bitset_list_destroy(&adjlist);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}

static igraph_error_t configuration_simple_undirected(
        igraph_t *graph,
        const igraph_vector_int_t *degseq) {

    igraph_vector_int_t stubs;
    igraph_bool_t graphical;
    igraph_integer_t vcount, stub_count;

    IGRAPH_CHECK(igraph_is_graphical(degseq, NULL, IGRAPH_SIMPLE_SW, &graphical));
    if (!graphical) {
        IGRAPH_ERROR("No simple undirected graph can realize the given degree sequence.", IGRAPH_EINVAL);
    }

    stub_count = igraph_vector_int_sum(degseq);
    vcount = igraph_vector_int_size(degseq);

    IGRAPH_VECTOR_INT_INIT_FINALLY(&stubs, stub_count);

    /* Fill stubs vector. */
    {
        igraph_integer_t k = 0;
        for (igraph_integer_t i = 0; i < vcount; ++i) {
            igraph_integer_t deg = VECTOR(*degseq)[i];
            for (igraph_integer_t j = 0; j < deg; ++j) {
                VECTOR(stubs)[k++] = i;
            }
        }
    }

    /* Tradeoff between speed and memory: Choose set vs bitset implementation. */
    if (vcount > 1024) {
        IGRAPH_CHECK(configuration_simple_undirected_set(degseq, &stubs, vcount, stub_count));
    } else {
        IGRAPH_CHECK(configuration_simple_undirected_bitset(&stubs, vcount, stub_count));
    }

    IGRAPH_CHECK(igraph_create(graph, &stubs, vcount, IGRAPH_UNDIRECTED));

    igraph_vector_int_destroy(&stubs);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}


static igraph_error_t configuration_simple_directed(
        igraph_t *graph,
        const igraph_vector_int_t *out_deg,
        const igraph_vector_int_t *in_deg) {

    igraph_vector_int_t out_stubs, in_stubs;
    igraph_vector_int_t edges;
    igraph_vector_int_t vertex_done;
    igraph_bool_t graphical;
    igraph_integer_t vcount, ecount;
    int iter = 0;

    IGRAPH_CHECK(igraph_is_graphical(out_deg, in_deg, IGRAPH_SIMPLE_SW, &graphical));
    if (!graphical) {
        IGRAPH_ERROR("No simple directed graph can realize the given degree sequence", IGRAPH_EINVAL);
    }

    ecount = igraph_vector_int_sum(out_deg);
    vcount = igraph_vector_int_size(out_deg);

    /* In the directed case, checking for multi-edges can be done efficiently for as
     * long as only one of the in-/out-stub vectors is shuffled. Here we shuffle
     * the out-stub vector, keeping in the in-stubs in their original order, thus
     * processing vertices to connect *to* in order. For each vertex v, we mark
     * vertex_done[v] to indicate that it has already been connected *to* the
     * current vertex. When moving on to the next vertex, instead of nulling the
     * vertex_done vector, we simply change the mark we use. */

    IGRAPH_VECTOR_INT_INIT_FINALLY(&edges, 2 * ecount);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&out_stubs, ecount);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&in_stubs, ecount);
    IGRAPH_VECTOR_INT_INIT_FINALLY(&vertex_done, vcount);

    /* Fill in- and out-stubs vectors. */
    {
        igraph_integer_t k = 0, l = 0;
        for (igraph_integer_t i = 0; i < vcount; ++i) {
            igraph_integer_t dout, din;

            dout = VECTOR(*out_deg)[i];
            for (igraph_integer_t j = 0; j < dout; ++j) {
                VECTOR(out_stubs)[k++] = i;
            }

            din  = VECTOR(*in_deg)[i];
            for (igraph_integer_t j = 0; j < din; ++j) {
                VECTOR(in_stubs)[l++] = i;
            }
        }
    }

    igraph_integer_t vertex_done_mark = 1;

    RNG_BEGIN();

    for (;;) {
        igraph_bool_t success = true;
        igraph_integer_t previous_to = -1;

        /* Shuffle out-stubs vector with Fisher-Yates and check for self-loops and multi-edges as we go. */
        for (igraph_integer_t i = 0; i < ecount; ++i) {
            igraph_integer_t k, from, to;

            k = RNG_INTEGER(i, ecount-1);
            SWAP_INT_ELEM(out_stubs, i, k);

            from = VECTOR(out_stubs)[i];
            to   = VECTOR(in_stubs)[i];

            /* self-loop, fail */
            if (to == from) {
                success = false;
                break;
            }

            /* have we moved on to the next vertex? */
            if (to != previous_to) {
                vertex_done_mark++;
                previous_to = to;
            }

            /* multi-edge, fail */
            if (VECTOR(vertex_done)[from] == vertex_done_mark) {
                success = false;
                break;
            }

            VECTOR(vertex_done)[from] = vertex_done_mark;
        }

        if (success) {
            break;
        }

        IGRAPH_ALLOW_INTERRUPTION_LIMITED(iter, 1 << 8);
    }

    RNG_END();

    for (igraph_integer_t i=0; i < ecount; i++) {
        VECTOR(edges)[2*i]   = VECTOR(out_stubs)[i];
        VECTOR(edges)[2*i+1] = VECTOR(in_stubs)[i];
    }

    igraph_vector_int_destroy(&vertex_done);
    igraph_vector_int_destroy(&in_stubs);
    igraph_vector_int_destroy(&out_stubs);
    IGRAPH_FINALLY_CLEAN(3);

    IGRAPH_CHECK(igraph_create(graph, &edges, vcount, IGRAPH_DIRECTED));

    igraph_vector_int_destroy(&edges);
    IGRAPH_FINALLY_CLEAN(1);

    return IGRAPH_SUCCESS;
}

#undef SWAP_INT_ELEM

igraph_error_t edge_switching(
        igraph_t *graph,
        const igraph_vector_int_t *out_seq,
        const igraph_vector_int_t *in_seq) {

    IGRAPH_CHECK(igraph_realize_degree_sequence(graph, out_seq, in_seq, IGRAPH_SIMPLE_SW, IGRAPH_REALIZE_DEGSEQ_INDEX));
    IGRAPH_FINALLY(igraph_destroy, graph);
    IGRAPH_CHECK(igraph_rewire(graph, 10 * igraph_ecount(graph), IGRAPH_REWIRING_SIMPLE));
    IGRAPH_FINALLY_CLEAN(1);
    return IGRAPH_SUCCESS;
}


/**
 * \ingroup generators
 * \function igraph_degree_sequence_game
 * \brief Generates a random graph with a given degree sequence.
 *
 * This function generates random graphs with a prescribed degree sequence.
 * Several sampling methods are available, which respect different constraints
 * (simple graph or multigraphs, connected graphs, etc.), and provide different
 * tradeoffs between performance and unbiased sampling. See Section 2.1 of
 * Horvát and Modes (2021) for an overview of sampling techniques for graphs
 * with fixed degrees.
 *
 * </para><para>
 * References:
 *
 * </para><para>
 * Fabien Viger, and Matthieu Latapy:
 * Efficient and Simple Generation of Random Simple Connected Graphs with Prescribed Degree Sequence,
 * Journal of Complex Networks 4, no. 1, pp. 15–37 (2015).
 * https://doi.org/10.1093/comnet/cnv013.
 *
 * </para><para>
 * Szabolcs Horvát, and Carl D Modes:
 * Connectedness Matters: Construction and Exact Random Sampling of Connected Networks,
 * Journal of Physics: Complexity 2, no. 1, pp. 015008 (2021).
 * https://doi.org/10.1088/2632-072x/abced5.
 *
 * \param graph Pointer to an uninitialized graph object.
 * \param out_deg The degree sequence for an undirected graph (if
 *        \p in_seq is \c NULL or of length zero), or the out-degree
 *        sequence of a directed graph (if \p in_deq is not
 *        of length zero).
 * \param in_deg It is either a zero-length vector or
 *        \c NULL (if an undirected
 *        graph is generated), or the in-degree sequence.
 * \param method The method to generate the graph. Possible values:
 *        \clist
 *          \cli IGRAPH_DEGSEQ_CONFIGURATION
 *          This method implements the configuration model.
 *          For undirected graphs, it puts all vertex IDs in a bag
 *          such that the multiplicity of a vertex in the bag is the same as
 *          its degree. Then it draws pairs from the bag until the bag becomes
 *          empty. This method may generate both loop (self) edges and multiple
 *          edges. For directed graphs, the algorithm is basically the same,
 *          but two separate bags are used for the in- and out-degrees.
 *          Undirected graphs are generated with probability proportional to
 *          <code>(\prod_{i&lt;j} A_{ij} ! \prod_i A_{ii} !!)^{-1}</code>,
 *          where \c A denotes the adjacency matrix and <code>!!</code> denotes
 *          the double factorial. Here \c A is assumed to have twice the number of
 *          self-loops on its diagonal.
 *          The corresponding  expression for directed graphs is
 *          <code>(\prod_{i,j} A_{ij}!)^{-1}</code>.
 *          Thus the probability of all simple graphs (which only have 0s and 1s
 *          in the adjacency matrix) is the same, while that of
 *          non-simple ones depends on their edge and self-loop multiplicities.
 *          \cli IGRAPH_DEGSEQ_CONFIGURATION_SIMPLE
 *          This method is identical to \c IGRAPH_DEGSEQ_CONFIGURATION, but if the
 *          generated graph is not simple, it rejects it and re-starts the
 *          generation. It generates all simple graphs with the same probability.
 *          \cli IGRAPH_DEGSEQ_FAST_HEUR_SIMPLE
 *          This method generates simple graphs.
 *          It is similar to \c IGRAPH_DEGSEQ_CONFIGURATION
 *          but tries to avoid multiple and loop edges and restarts the
 *          generation from scratch if it gets stuck. It can generate all simple
 *          realizations of a degree sequence, but it is not guaranteed
 *          to sample them uniformly. This method is relatively fast and it will
 *          eventually succeed if the provided degree sequence is graphical,
 *          but there is no upper bound on the number of iterations.
 *          \cli IGRAPH_DEGSEQ_EDGE_SWITCHING_SIMPLE
 *          This is an MCMC sampler based on degree-preserving edge switches.
 *          It generates simple undirected or directed graphs.
 *          It uses \ref igraph_realize_degree_sequence() to construct an initial
 *          graph, then rewires it using \ref igraph_rewire().
 *          \cli IGRAPH_DEGSEQ_VL
 *          This method samples undirected \em connected graphs approximately
 *          uniformly. It is a Monte Carlo method based on degree-preserving
 *          edge switches.
 *          This generator should be favoured if undirected and connected
 *          graphs are to be generated and execution time is not a concern.
 *          igraph uses the original implementation of Fabien Viger; for the algorithm,
 *          see https://www-complexnetworks.lip6.fr/~latapy/FV/generation.html
 *          and the paper https://arxiv.org/abs/cs/0502085
 *        \endclist
 * \return Error code:
 *          \c IGRAPH_ENOMEM: there is not enough
 *           memory to perform the operation.
 *          \c IGRAPH_EINVAL: invalid method parameter, or
 *           invalid in- and/or out-degree vectors. The degree vectors
 *           should be non-negative, \p out_deg should sum
 *           up to an even integer for undirected graphs; the length
 *           and sum of \p out_deg and
 *           \p in_deg
 *           should match for directed graphs.
 *
 * Time complexity: O(|V|+|E|), the number of vertices plus the number of edges
 * for \c IGRAPH_DEGSEQ_CONFIGURATION and \c IGRAPH_DEGSEQ_EDGE_SWITCHING_SIMPLE.
 * The time complexity of the other modes is not known.
 *
 * \sa \ref igraph_is_graphical() to determine if there exist graphs with a certain
 * degree sequence; \ref igraph_erdos_renyi_game_gnm() to generate graphs with a
 * fixed number of edges, without any degree constraints; \ref igraph_chung_lu_game()
 * and \ref igraph_static_fitness_game() to sample random graphs with a prescribed
 * \em expected degree sequence (but variable actual degrees);
 * \ref igraph_realize_degree_sequence() and \ref igraph_realize_bipartite_degree_sequence()
 * to generate a single (non-random) graph with given degrees.
 *
 * \example examples/simple/igraph_degree_sequence_game.c
 */

igraph_error_t igraph_degree_sequence_game(
        igraph_t *graph,
        const igraph_vector_int_t *out_deg,
        const igraph_vector_int_t *in_deg,
        igraph_degseq_t method) {

    if (in_deg && igraph_vector_int_empty(in_deg) && !igraph_vector_int_empty(out_deg)) {
        in_deg = NULL;
    }

    switch (method) {
    case IGRAPH_DEGSEQ_CONFIGURATION:
        return configuration(graph, out_deg, in_deg);

    case IGRAPH_DEGSEQ_VL:
        return igraph_i_degree_sequence_game_vl(graph, out_deg, in_deg);

    case IGRAPH_DEGSEQ_FAST_HEUR_SIMPLE:
        if (! in_deg) {
            return fast_heur_undirected(graph, out_deg);
        } else {
            return fast_heur_directed(graph, out_deg, in_deg);
        }

    case IGRAPH_DEGSEQ_CONFIGURATION_SIMPLE:
        if (! in_deg) {
            return configuration_simple_undirected(graph, out_deg);
        } else {
            return configuration_simple_directed(graph, out_deg, in_deg);
        }

    case IGRAPH_DEGSEQ_EDGE_SWITCHING_SIMPLE:
        return edge_switching(graph, out_deg, in_deg);

    default:
        IGRAPH_ERROR("Invalid degree sequence game method.", IGRAPH_EINVAL);
    }
}
