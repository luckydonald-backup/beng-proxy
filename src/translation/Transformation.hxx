/*
 * Transformations which can be applied to resources.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_TRANSFORMATION_HXX
#define BENG_TRANSFORMATION_HXX

#include "ResourceAddress.hxx"

#include <inline/compiler.h>

#include <assert.h>

struct pool;
class AllocatorPtr;

struct Transformation {
    Transformation *next;

    enum class Type {
        PROCESS,
        PROCESS_CSS,
        PROCESS_TEXT,
        FILTER,
    } type;

    union {
        struct {
            unsigned options;
        } processor;

        struct {
            unsigned options;
        } css_processor;

        struct {
            ResourceAddress address;

            /**
             * Send the X-CM4all-BENG-User header to the filter?
             */
            bool reveal_user;
        } filter;
    } u;

    /**
     * Returns true if the chain contains at least one "PROCESS"
     * transformation.
     */
    gcc_pure
    static bool HasProcessor(const Transformation *head);

    /**
     * Returns true if the first "PROCESS" transformation in the chain (if
     * any) includes the "CONTAINER" processor option.
     */
    gcc_pure
    static bool IsContainer(const Transformation *head);

    /**
     * Does this transformation need to be expanded with
     * transformation_expand()?
     */
    gcc_pure
    bool IsExpandable() const {
        return type == Type::FILTER &&
            u.filter.address.IsExpandable();
    }

    /**
     * Does any transformation in the linked list need to be expanded with
     * transformation_expand()?
     */
    gcc_pure
    bool IsChainExpandable() const;

    gcc_malloc
    Transformation *Dup(AllocatorPtr alloc) const;

    gcc_malloc
    static Transformation *DupChain(AllocatorPtr alloc,
                                    const Transformation *src);

    /**
     * Expand the strings in this transformation (not following the linked
     * lits) with the specified regex result.
     *
     * Throws std::runtime_error on error.
     */
    void Expand(AllocatorPtr alloc, const MatchInfo &match_info);

    /**
     * The same as Expand(), but expand all transformations in the
     * linked list.
     */
    void ExpandChain(AllocatorPtr alloc, const MatchInfo &match_info);
};

#endif
