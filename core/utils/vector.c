#include <stddef.h>
#include <stdlib.h>
#include <assert.h>
#include "vector.h"

#define REQUIRED_VOTES_TO_SHRINK 15
#define CAPACITY_TO_SIZE_RATIO_FOR_SHRINK_VOTE 4
#define SCALE_FACTOR 2

static void vector_resize(vector_t* v, size_t new_capacity);

/**
 * Allocate and initialize a new vector.
 * @param initial_capacity The desired initial capacity to allocate.
 *  Must be more than 0
 */
vector_t vector_new(size_t initial_capacity) {
    assert(initial_capacity > 0);
    void** start = (void**) malloc(initial_capacity * sizeof(void*));
    assert(start);
    return (vector_t) {
        .start = start,
        .next = start,
        .end = start + initial_capacity,
        .votes_required = REQUIRED_VOTES_TO_SHRINK,
        .votes = 0
    };
}

/**
 * Free the memory held by the given vector, invalidating it.
 * @param v Any vector.
 */
void vector_free(vector_t* v) {
    assert(v);
    free(v->start);
}

/**
 * Add the given element to the vector.
 * @param v A vector that is to grow.
 * @param element An element that the vector should contain.
 */
void vector_push(vector_t* v, void* element) {
    if (v->next == v->end) {
        v->votes_required++;
        vector_resize(v, (v->end - v->start) * SCALE_FACTOR);
    }
    *(v->next++) = element;
}

/**
 * Add all elements of the given array to the vector. Elements should be
 * non-null.
 * @param v A vector that is to grow.
 * @param array An array of items to be added to the vector.
 * @param size The size of the given array.
 */
void vector_pushall(vector_t* v, void** array, size_t size) {
    void** required_end = v->next + size;
    if (required_end > v->end) {
        vector_resize(v, (required_end - v->start) * SCALE_FACTOR);
    }
    for (size_t i = 0; i < size; i++) {
        assert(array[i]);
        v->next[i] = array[i];
    }
    v->next += size;
}

/**
 * Remove and return some pointer that is contained in the given vector,
 * or return NULL if the given vector is empty.
 * @param v Any vector.
 */
void* vector_pop(vector_t* v) {
    if (v->next == v->start) {
        if (v->votes >= v->votes_required) {
            size_t new_capacity = (v->end - v->start) / SCALE_FACTOR;
            if (new_capacity > 0) {
                vector_resize(v, new_capacity);
            }
        }
        return NULL;
    }
    return *(--v->next);
}

/**
 * Vote on whether this vector ought to have a smaller memory footprint.
 */
void vector_vote(vector_t* v) {
    size_t size = v->next - v->start;
    int vote = size * CAPACITY_TO_SIZE_RATIO_FOR_SHRINK_VOTE <= (size_t) (v->end - v->start);
    v->votes = vote * v->votes + vote;
}

/**
 * Resets the capacity of the given vector without otherwise altering its
 * observable state.
 * @param v A vector that should have a different capacity.
 */
static void vector_resize(vector_t* v, size_t new_capacity) {
    if (new_capacity == 0) {
        // Don't shrink the queue further
        return;
    }
    size_t size = v->next - v->start;
    assert(size <= new_capacity);
    void** start = (void**) realloc(v->start, new_capacity * sizeof(void*));
    assert(start);
    v->votes = 0;
    v->start = start;
    v->next = start + size;
    v->end = start + new_capacity;
}
