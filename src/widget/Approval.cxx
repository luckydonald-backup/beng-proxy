/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Approval.hxx"
#include "Class.hxx"
#include "Widget.hxx"

bool
widget_init_approval(Widget *widget, bool self_container)
{
    assert(widget != NULL);
    assert(widget->parent != NULL);
    assert(widget->approval == Widget::Approval::GIVEN);

    const Widget *parent = widget->parent;

    if (!self_container) {
        if (widget_class_has_groups(parent->cls))
            /* the container limits the groups; postpone a check until
               we know the widget's group */
            widget->approval = Widget::Approval::UNKNOWN;

        return true;
    }

    if (parent->class_name != NULL &&
        strcmp(parent->class_name, widget->class_name) == 0)
        /* approved by SELF_CONTAINER */
        return true;

    /* failed the SELF_CONTAINER test */

    if (widget_class_has_groups(parent->cls)) {
        /* the container allows a set of groups - postpone the
           approval check until we know this widget's group
           (if any) */
        widget->approval = Widget::Approval::UNKNOWN;
        return true;
    } else {
        /* the container does not allow any additional group,
           which means this widget's approval check has
           ultimately failed */
        widget->approval = Widget::Approval::DENIED;
        return false;
    }
}

static inline bool
widget_check_group_approval(const Widget *widget)
{
    assert(widget != NULL);
    assert(widget->parent != NULL);

    if (widget->parent->cls == NULL ||
        !widget_class_has_groups(widget->parent->cls))
        return true;

    if (widget->cls == NULL)
        return false;

    return widget_class_may_embed(widget->parent->cls, widget->cls);
}

bool
widget_check_approval(Widget *widget)
{
    assert(widget != NULL);
    assert(widget->parent != NULL);

    if (widget->approval == Widget::Approval::UNKNOWN)
        widget->approval = widget_check_group_approval(widget)
            ? Widget::Approval::GIVEN
            : Widget::Approval::DENIED;

    return widget->approval == Widget::Approval::GIVEN;
}
