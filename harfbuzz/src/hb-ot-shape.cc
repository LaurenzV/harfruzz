/*
 * Copyright © 2009,2010  Red Hat, Inc.
 * Copyright © 2010,2011,2012  Google, Inc.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Red Hat Author(s): Behdad Esfahbod
 * Google Author(s): Behdad Esfahbod
 */

#include "hb.hh"

#include "hb-shape-plan.hh"
#include "hb-ot-shape.hh"
#include "hb-ot-layout.hh"

#include "hb-face.hh"

#include "hb-aat-layout.hh"

extern "C" {
void rb_set_unicode_props(rb_buffer_t *buffer);
void rb_insert_dotted_circle(rb_buffer_t *buffer, rb_face_t *face);
void rb_form_clusters(rb_buffer_t *buffer);
void rb_ensure_native_direction(rb_buffer_t *buffer);
rb_codepoint_t rb_vert_char_for(rb_codepoint_t u);
void rb_ot_zero_width_default_ignorables(const rb_buffer_t *buffer);
void rb_ot_hide_default_ignorables(rb_buffer_t *buffer, rb_face_t *face);
void rb_ot_map_glyphs_fast(rb_buffer_t *buffer);
void rb_synthesize_glyph_classes(rb_buffer_t *buffer);
void rb_zero_mark_widths_by_gdef(rb_buffer_t *buffer, bool adjust_offsets);
void rb_propagate_flags(rb_buffer_t *buffer);
void rb_ot_shape_normalize(const rb_ot_shape_plan_t *plan, rb_buffer_t *buffer, rb_face_t *face);
void rb_ot_shape_fallback_mark_position(const rb_ot_shape_plan_t *plan, rb_face_t *face, rb_buffer_t *buffer, bool adjust_offsets_when_zeroing);
void rb_ot_shape_fallback_mark_position_recategorize_marks(const rb_ot_shape_plan_t *plan, rb_face_t *face, rb_buffer_t *buffer);
void rb_ot_shape_fallback_kern(const rb_ot_shape_plan_t *plan, rb_face_t *face, rb_buffer_t *buffer);
void rb_ot_shape_fallback_spaces(const rb_ot_shape_plan_t *plan, rb_face_t *face, rb_buffer_t *buffer);
}

const rb_ot_complex_shaper_t *rb_ot_shape_plan_get_ot_complex_shaper(const rb_ot_shape_plan_t *plan) { return plan->shaper; }
const rb_ot_map_t *rb_ot_shape_plan_get_ot_map(const rb_ot_shape_plan_t *plan) { return plan->map; }
const void *rb_ot_shape_plan_get_data(const rb_ot_shape_plan_t *plan) { return plan->data; }
rb_script_t rb_ot_shape_plan_get_script(const rb_ot_shape_plan_t *plan) { return plan->props.script; }
rb_direction_t rb_ot_shape_plan_get_direction(const rb_ot_shape_plan_t *plan) { return plan->props.direction; }
bool rb_ot_shape_plan_has_gpos_mark(const rb_ot_shape_plan_t *plan) { return plan->has_gpos_mark; }
rb_ot_map_builder_t *rb_ot_shape_planner_get_ot_map(rb_ot_shape_planner_t *planner) { return planner->map; }
rb_script_t rb_ot_shape_planner_get_script(const rb_ot_shape_planner_t *planner) { return planner->props.script; }
rb_direction_t rb_ot_shape_planner_get_direction(const rb_ot_shape_planner_t *planner) { return planner->props.direction; }

static inline bool rb_apply_morx(rb_face_t *face, const rb_segment_properties_t *props)
{
    /* https://github.com/harfbuzz/harfbuzz/issues/2124 */
    return rb_aat_layout_has_substitution(face) &&
           (RB_DIRECTION_IS_HORIZONTAL(props->direction) || !rb_ot_layout_has_substitution(face));
}

/**
 * SECTION:hb-ot-shape
 * @title: hb-ot-shape
 * @short_description: OpenType shaping support
 * @include: hb-ot.h
 *
 * Support functions for OpenType shaping related queries.
 **/

static void rb_ot_shape_collect_features(rb_ot_shape_planner_t *planner,
                                         const rb_feature_t *user_features,
                                         unsigned int num_user_features);

rb_ot_shape_planner_t::rb_ot_shape_planner_t(rb_face_t *face, const rb_segment_properties_t *props)
    : face(face)
    , props(*props)
    , map(rb_ot_map_builder_create(face, props))
    , aat_map(face, props)
    , apply_morx(rb_apply_morx(face, props))
{
    shaper = rb_ot_shape_complex_categorize(this);

    script_zero_marks = rb_ot_complex_shaper_get_zero_width_marks_mode(shaper) != RB_OT_SHAPE_ZERO_WIDTH_MARKS_NONE;
    script_fallback_mark_positioning = rb_ot_complex_shaper_get_fallback_position(shaper);

    /* https://github.com/harfbuzz/harfbuzz/issues/1528 */

    if (apply_morx) {
        shaper = rb_ot_complex_shaper_reconsider_shaper_if_applying_morx(shaper);
    }
}

rb_ot_shape_planner_t::~rb_ot_shape_planner_t()
{
    rb_ot_map_builder_destroy(map);
}

void rb_ot_shape_planner_t::compile(rb_ot_shape_plan_t &plan, unsigned int *variations_index)
{
    plan.props = props;
    plan.shaper = shaper;
    rb_ot_map_builder_compile(map, plan.map, variations_index);
    if (apply_morx)
        aat_map.compile(plan.aat_map);

    plan.frac_mask = rb_ot_map_get_1_mask(plan.map, RB_TAG('f', 'r', 'a', 'c'));
    plan.numr_mask = rb_ot_map_get_1_mask(plan.map, RB_TAG('n', 'u', 'm', 'r'));
    plan.dnom_mask = rb_ot_map_get_1_mask(plan.map, RB_TAG('d', 'n', 'o', 'm'));
    plan.has_frac = plan.frac_mask || (plan.numr_mask && plan.dnom_mask);

    plan.rtlm_mask = rb_ot_map_get_1_mask(plan.map, RB_TAG('r', 't', 'l', 'm'));
    plan.has_vert = !!rb_ot_map_get_1_mask(plan.map, RB_TAG('v', 'e', 'r', 't'));

    unsigned int shift;
    rb_tag_t kern_tag = RB_DIRECTION_IS_HORIZONTAL(props.direction) ? RB_TAG('k', 'e', 'r', 'n') : RB_TAG('v', 'k', 'r', 'n');
    plan.kern_mask = rb_ot_map_get_mask(plan.map, kern_tag, &shift);
    plan.requested_kerning = !!plan.kern_mask;
    plan.trak_mask = rb_ot_map_get_mask(plan.map, RB_TAG('t', 'r', 'a', 'k'), &shift);
    plan.requested_tracking = !!plan.trak_mask;

    rb_tag_t gpos_tag = rb_ot_complex_shaper_get_gpos_tag(plan.shaper);
    bool has_gpos_kern = rb_ot_map_get_feature_index(plan.map, 1, kern_tag) != RB_OT_LAYOUT_NO_FEATURE_INDEX;
    bool disable_gpos = gpos_tag && gpos_tag != rb_ot_map_get_chosen_script(plan.map, 1);

    /*
     * Decide who provides glyph classes. GDEF or Unicode.
     */

    if (!rb_ot_layout_has_glyph_classes(face))
        plan.fallback_glyph_classes = true;

    /*
     * Decide who does substitutions. GSUB, morx, or fallback.
     */

    plan.apply_morx = apply_morx;

    /*
     * Decide who does positioning. GPOS, kerx, kern, or fallback.
     */

    if (rb_aat_layout_has_positioning(face))
        plan.apply_kerx = true;
    else if (!apply_morx && !disable_gpos && rb_ot_layout_has_positioning(face))
        plan.apply_gpos = true;

    if (!plan.apply_kerx && (!has_gpos_kern || !plan.apply_gpos)) {
        /* Apparently Apple applies kerx if GPOS kern was not applied. */
        if (rb_aat_layout_has_positioning(face))
            plan.apply_kerx = true;
        else if (rb_ot_layout_has_kerning(face))
            plan.apply_kern = true;
    }

    plan.zero_marks = script_zero_marks && !plan.apply_kerx && (!plan.apply_kern || !rb_ot_layout_has_machine_kerning(face));
    plan.has_gpos_mark = !!rb_ot_map_get_1_mask(plan.map, RB_TAG('m', 'a', 'r', 'k'));

    plan.adjust_mark_positioning_when_zeroing =
        !plan.apply_gpos && !plan.apply_kerx && (!plan.apply_kern || !rb_ot_layout_has_cross_kerning(face));

    plan.fallback_mark_positioning = plan.adjust_mark_positioning_when_zeroing && script_fallback_mark_positioning;

    /* Currently we always apply trak. */
    plan.apply_trak = plan.requested_tracking && rb_aat_layout_has_tracking(face);
}

bool rb_ot_shape_plan_t::init0(rb_face_t *face,
                               const rb_segment_properties_t *props,
                               const rb_feature_t *user_features,
                               unsigned int num_user_features,
                               unsigned int *variations_index)
{
    map = rb_ot_map_create();
    aat_map.init();

    rb_ot_shape_planner_t planner(face, props);

    rb_ot_shape_collect_features(&planner, user_features, num_user_features);

    planner.compile(*this, variations_index);

    if (unlikely(!rb_ot_complex_shaper_data_create(shaper, this, &data))) {
        rb_ot_map_destroy(map);
        aat_map.fini();
        return false;
    }

    return true;
}

void rb_ot_shape_plan_t::fini()
{
    rb_ot_complex_shaper_data_destroy(shaper, const_cast<void *>(data));

    rb_ot_map_destroy(map);
    aat_map.fini();
}

void rb_ot_shape_plan_t::substitute(rb_face_t *face, rb_buffer_t *buffer) const
{
    if (unlikely(apply_morx))
        rb_aat_layout_substitute(this, face, buffer);
    else
        rb_ot_layout_substitute(this, map, face, buffer);
}

void rb_ot_shape_plan_t::position(rb_face_t *face, rb_buffer_t *buffer) const
{
    if (this->apply_gpos)
        rb_ot_layout_position(this, map, face, buffer);
    else if (this->apply_kerx)
        rb_aat_layout_position(this, face, buffer);
    else if (this->apply_kern)
        rb_ot_layout_kern(this, face, buffer);
    else
        rb_ot_shape_fallback_kern(this, face, buffer);

    if (this->apply_trak)
        rb_aat_layout_track(this, face, buffer);
}

static const rb_ot_map_feature_t common_features[] = {
    {RB_TAG('a', 'b', 'v', 'm'), F_GLOBAL},
    {RB_TAG('b', 'l', 'w', 'm'), F_GLOBAL},
    {RB_TAG('c', 'c', 'm', 'p'), F_GLOBAL},
    {RB_TAG('l', 'o', 'c', 'l'), F_GLOBAL},
    {RB_TAG('m', 'a', 'r', 'k'), F_GLOBAL_MANUAL_JOINERS},
    {RB_TAG('m', 'k', 'm', 'k'), F_GLOBAL_MANUAL_JOINERS},
    {RB_TAG('r', 'l', 'i', 'g'), F_GLOBAL},
};

static const rb_ot_map_feature_t horizontal_features[] = {
    {RB_TAG('c', 'a', 'l', 't'), F_GLOBAL},
    {RB_TAG('c', 'l', 'i', 'g'), F_GLOBAL},
    {RB_TAG('c', 'u', 'r', 's'), F_GLOBAL},
    {RB_TAG('d', 'i', 's', 't'), F_GLOBAL},
    {RB_TAG('k', 'e', 'r', 'n'), F_GLOBAL_HAS_FALLBACK},
    {RB_TAG('l', 'i', 'g', 'a'), F_GLOBAL},
    {RB_TAG('r', 'c', 'l', 't'), F_GLOBAL},
};

static void rb_ot_shape_collect_features(rb_ot_shape_planner_t *planner,
                                         const rb_feature_t *user_features,
                                         unsigned int num_user_features)
{
    rb_ot_map_builder_t *map = planner->map;

    rb_ot_map_builder_enable_feature(map, RB_TAG('r', 'v', 'r', 'n'), F_NONE, 1);
    rb_ot_map_builder_add_gsub_pause(map, nullptr);

    switch (planner->props.direction) {
    case RB_DIRECTION_LTR:
        rb_ot_map_builder_enable_feature(map, RB_TAG('l', 't', 'r', 'a'), F_NONE, 1);
        rb_ot_map_builder_enable_feature(map, RB_TAG('l', 't', 'r', 'm'), F_NONE, 1);
        break;
    case RB_DIRECTION_RTL:
        rb_ot_map_builder_enable_feature(map, RB_TAG('r', 't', 'l', 'a'), F_NONE, 1);
        rb_ot_map_builder_add_feature(map, RB_TAG('r', 't', 'l', 'm'), F_NONE, 1);
        break;
    case RB_DIRECTION_TTB:
    case RB_DIRECTION_BTT:
    case RB_DIRECTION_INVALID:
    default:
        break;
    }

    /* Automatic fractions. */
    rb_ot_map_builder_add_feature(map, RB_TAG('f', 'r', 'a', 'c'), F_NONE, 1);
    rb_ot_map_builder_add_feature(map, RB_TAG('n', 'u', 'm', 'r'), F_NONE, 1);
    rb_ot_map_builder_add_feature(map, RB_TAG('d', 'n', 'o', 'm'), F_NONE, 1);

    /* Random! */
    rb_ot_map_builder_enable_feature(map, RB_TAG('r', 'a', 'n', 'd'), F_RANDOM, RB_OT_MAP_MAX_VALUE);

    /* Tracking.  We enable dummy feature here just to allow disabling
     * AAT 'trak' table using features.
     * https://github.com/harfbuzz/harfbuzz/issues/1303 */
    rb_ot_map_builder_enable_feature(map, RB_TAG('t', 'r', 'a', 'k'), F_HAS_FALLBACK, 1);

    rb_ot_map_builder_enable_feature(map, RB_TAG('H', 'A', 'R', 'F'), F_NONE, 1);

    rb_ot_complex_shaper_collect_features(planner->shaper, planner);

    rb_ot_map_builder_enable_feature(map, RB_TAG('B', 'U', 'Z', 'Z'), F_NONE, 1);

    for (unsigned int i = 0; i < ARRAY_LENGTH(common_features); i++)
        rb_ot_map_builder_add_feature(map, common_features[i].tag, common_features[i].flags, 1);

    if (RB_DIRECTION_IS_HORIZONTAL(planner->props.direction))
        for (unsigned int i = 0; i < ARRAY_LENGTH(horizontal_features); i++)
            rb_ot_map_builder_add_feature(map, horizontal_features[i].tag, horizontal_features[i].flags, 1);
    else {
        /* We really want to find a 'vert' feature if there's any in the font, no
         * matter which script/langsys it is listed (or not) under.
         * See various bugs referenced from:
         * https://github.com/harfbuzz/harfbuzz/issues/63 */
        rb_ot_map_builder_enable_feature(map, RB_TAG('v', 'e', 'r', 't'), F_GLOBAL_SEARCH, 1);
    }

    for (unsigned int i = 0; i < num_user_features; i++) {
        const rb_feature_t *feature = &user_features[i];
        rb_ot_map_builder_add_feature(map,
                                      feature->tag,
                                      (feature->start == RB_FEATURE_GLOBAL_START && feature->end == RB_FEATURE_GLOBAL_END) ? F_GLOBAL : F_NONE,
                                      feature->value);
    }

    if (planner->apply_morx) {
        rb_aat_map_builder_t *aat_map = &planner->aat_map;
        for (unsigned int i = 0; i < num_user_features; i++) {
            const rb_feature_t *feature = &user_features[i];
            aat_map->add_feature(feature->tag, feature->value);
        }
    }

    rb_ot_complex_shaper_override_features(planner->shaper, planner);
}

/*
 * shaper
 */

struct rb_ot_shape_context_t
{
    rb_ot_shape_plan_t *plan;
    rb_face_t *face;
    rb_buffer_t *buffer;
    const rb_feature_t *user_features;
    unsigned int num_user_features;

    /* Transient stuff */
    rb_direction_t target_direction;
};

/*
 * Substitute
 */

static inline void rb_ot_rotate_chars(const rb_ot_shape_context_t *c)
{
    rb_buffer_t *buffer = c->buffer;
    unsigned int count = rb_buffer_get_length(buffer);
    rb_glyph_info_t *info = rb_buffer_get_glyph_infos(buffer);

    if (RB_DIRECTION_IS_BACKWARD(c->target_direction)) {
        rb_mask_t rtlm_mask = c->plan->rtlm_mask;

        for (unsigned int i = 0; i < count; i++) {
            rb_codepoint_t codepoint = rb_ucd_mirroring(info[i].codepoint);
            if (unlikely(codepoint != info[i].codepoint && rb_face_has_glyph(c->face, codepoint)))
                info[i].codepoint = codepoint;
            else
                info[i].mask |= rtlm_mask;
        }
    }

    if (RB_DIRECTION_IS_VERTICAL(c->target_direction) && !c->plan->has_vert) {
        for (unsigned int i = 0; i < count; i++) {
            rb_codepoint_t codepoint = rb_vert_char_for(info[i].codepoint);
            if (unlikely(codepoint != info[i].codepoint && rb_face_has_glyph(c->face, codepoint)))
                info[i].codepoint = codepoint;
        }
    }
}

static inline void rb_ot_shape_setup_masks_fraction(const rb_ot_shape_context_t *c)
{
    if (!(rb_buffer_get_scratch_flags(c->buffer) & RB_BUFFER_SCRATCH_FLAG_HAS_NON_ASCII) || !c->plan->has_frac)
        return;

    rb_buffer_t *buffer = c->buffer;

    rb_mask_t pre_mask, post_mask;
    if (RB_DIRECTION_IS_FORWARD(rb_buffer_get_direction(c->buffer))) {
        pre_mask = c->plan->numr_mask | c->plan->frac_mask;
        post_mask = c->plan->frac_mask | c->plan->dnom_mask;
    } else {
        pre_mask = c->plan->frac_mask | c->plan->dnom_mask;
        post_mask = c->plan->numr_mask | c->plan->frac_mask;
    }

    unsigned int count = rb_buffer_get_length(buffer);
    rb_glyph_info_t *info = rb_buffer_get_glyph_infos(buffer);
    for (unsigned int i = 0; i < count; i++) {
        if (info[i].codepoint == 0x2044u) /* FRACTION SLASH */
        {
            unsigned int start = i, end = i + 1;
            while (start &&
                   rb_glyph_info_get_general_category(&info[start - 1]) == RB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER)
                start--;
            while (end < count &&
                   rb_glyph_info_get_general_category(&info[end]) == RB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER)
                end++;

            rb_buffer_unsafe_to_break(buffer, start, end);

            for (unsigned int j = start; j < i; j++)
                info[j].mask |= pre_mask;
            info[i].mask |= c->plan->frac_mask;
            for (unsigned int j = i + 1; j < end; j++)
                info[j].mask |= post_mask;

            i = end - 1;
        }
    }
}

static inline void rb_ot_shape_initialize_masks(const rb_ot_shape_context_t *c)
{
    rb_ot_map_t *map = c->plan->map;

    rb_mask_t global_mask = rb_ot_map_get_global_mask(map);
    rb_buffer_reset_masks(c->buffer, global_mask);
}

static inline void rb_ot_shape_setup_masks(const rb_ot_shape_context_t *c)
{
    rb_ot_map_t *map = c->plan->map;
    rb_buffer_t *buffer = c->buffer;

    rb_ot_shape_setup_masks_fraction(c);

    rb_ot_complex_shaper_setup_masks(c->plan->shaper, c->plan, buffer, c->face);

    for (unsigned int i = 0; i < c->num_user_features; i++) {
        const rb_feature_t *feature = &c->user_features[i];
        if (!(feature->start == RB_FEATURE_GLOBAL_START && feature->end == RB_FEATURE_GLOBAL_END)) {
            unsigned int shift;
            rb_mask_t mask = rb_ot_map_get_mask(map, feature->tag, &shift);
            rb_buffer_set_masks(buffer, feature->value << shift, mask, feature->start, feature->end);
        }
    }
}

static inline void rb_ot_substitute_default(const rb_ot_shape_context_t *c)
{
    rb_buffer_t *buffer = c->buffer;

    rb_ot_rotate_chars(c);

    rb_ot_shape_normalize(c->plan, buffer, c->face);

    rb_ot_shape_setup_masks(c);

    /* This is unfortunate to go here, but necessary... */
    if (c->plan->fallback_mark_positioning)
        rb_ot_shape_fallback_mark_position_recategorize_marks(c->plan, c->face, buffer);

    rb_ot_map_glyphs_fast(buffer);
}

static inline void rb_ot_substitute_complex(const rb_ot_shape_context_t *c)
{
    rb_buffer_t *buffer = c->buffer;

    rb_ot_layout_substitute_start(c->face, buffer);

    if (c->plan->fallback_glyph_classes)
        rb_synthesize_glyph_classes(c->buffer);

    c->plan->substitute(c->face, buffer);
}

static inline void rb_ot_substitute_pre(const rb_ot_shape_context_t *c)
{
    rb_ot_substitute_default(c);
    rb_ot_substitute_complex(c);
}

static inline void rb_ot_substitute_post(const rb_ot_shape_context_t *c)
{
    rb_ot_hide_default_ignorables(c->buffer, c->face);
    if (c->plan->apply_morx)
        rb_aat_layout_remove_deleted_glyphs(c->buffer);

    rb_ot_complex_shaper_postprocess_glyphs(c->plan->shaper, c->plan, c->buffer, c->face);
}

/*
 * Position
 */

static inline void rb_ot_position_default(const rb_ot_shape_context_t *c)
{
    rb_direction_t direction = rb_buffer_get_direction(c->buffer);
    unsigned int count = rb_buffer_get_length(c->buffer);
    rb_glyph_info_t *info = rb_buffer_get_glyph_infos(c->buffer);
    rb_glyph_position_t *pos = rb_buffer_get_glyph_positions(c->buffer);

    if (RB_DIRECTION_IS_HORIZONTAL(direction)) {
        rb_face_get_glyph_h_advances(
            c->face, count, &info[0].codepoint, sizeof(info[0]), &pos[0].x_advance, sizeof(pos[0]));
    } else {
        rb_face_get_glyph_v_advances(
            c->face, count, &info[0].codepoint, sizeof(info[0]), &pos[0].y_advance, sizeof(pos[0]));
        for (unsigned int i = 0; i < count; i++) {
            rb_face_subtract_glyph_v_origin(c->face, info[i].codepoint, &pos[i].x_offset, &pos[i].y_offset);
        }
    }
    if (rb_buffer_get_scratch_flags(c->buffer) & RB_BUFFER_SCRATCH_FLAG_HAS_SPACE_FALLBACK)
        rb_ot_shape_fallback_spaces(c->plan, c->face, c->buffer);
}

static inline void rb_ot_position_complex(const rb_ot_shape_context_t *c)
{
    /* If the font has no GPOS and direction is forward, then when
     * zeroing mark widths, we shift the mark with it, such that the
     * mark is positioned hanging over the previous glyph.  When
     * direction is backward we don't shift and it will end up
     * hanging over the next glyph after the final reordering.
     *
     * Note: If fallback positinoing happens, we don't care about
     * this as it will be overriden.
     */
    bool adjust_offsets_when_zeroing =
        c->plan->adjust_mark_positioning_when_zeroing && RB_DIRECTION_IS_FORWARD(rb_buffer_get_direction(c->buffer));

    /* We change glyph origin to what GPOS expects (horizontal), apply GPOS, change it back. */

    rb_ot_layout_position_start(c->face, c->buffer);

    if (c->plan->zero_marks)
        switch (rb_ot_complex_shaper_get_zero_width_marks_mode(c->plan->shaper)) {
        case RB_OT_SHAPE_ZERO_WIDTH_MARKS_BY_GDEF_EARLY:
            rb_zero_mark_widths_by_gdef(c->buffer, adjust_offsets_when_zeroing);
            break;

        default:
        case RB_OT_SHAPE_ZERO_WIDTH_MARKS_NONE:
        case RB_OT_SHAPE_ZERO_WIDTH_MARKS_BY_GDEF_LATE:
            break;
        }

    c->plan->position(c->face, c->buffer);

    if (c->plan->zero_marks)
        switch (rb_ot_complex_shaper_get_zero_width_marks_mode(c->plan->shaper)) {
        case RB_OT_SHAPE_ZERO_WIDTH_MARKS_BY_GDEF_LATE:
            rb_zero_mark_widths_by_gdef(c->buffer, adjust_offsets_when_zeroing);
            break;

        default:
        case RB_OT_SHAPE_ZERO_WIDTH_MARKS_NONE:
        case RB_OT_SHAPE_ZERO_WIDTH_MARKS_BY_GDEF_EARLY:
            break;
        }

    /* Finish off.  Has to follow a certain order. */
    rb_ot_layout_position_finish_advances(c->face, c->buffer);
    rb_ot_zero_width_default_ignorables(c->buffer);
    if (c->plan->apply_morx)
        rb_aat_layout_zero_width_deleted_glyphs(c->buffer);
    rb_ot_layout_position_finish_offsets(c->face, c->buffer);

    if (c->plan->fallback_mark_positioning)
        rb_ot_shape_fallback_mark_position(c->plan, c->face, c->buffer, adjust_offsets_when_zeroing);
}

static inline void rb_ot_position(const rb_ot_shape_context_t *c)
{
    rb_buffer_clear_positions(c->buffer);

    rb_ot_position_default(c);

    rb_ot_position_complex(c);

    if (RB_DIRECTION_IS_BACKWARD(rb_buffer_get_direction(c->buffer)))
        rb_buffer_reverse(c->buffer);
}

/* Pull it all together! */

static void rb_ot_shape_internal(rb_ot_shape_context_t *c)
{
    rb_buffer_set_scratch_flags(c->buffer, RB_BUFFER_SCRATCH_FLAG_DEFAULT);
    if (likely(!rb_unsigned_mul_overflows(rb_buffer_get_length(c->buffer), RB_BUFFER_MAX_LEN_FACTOR))) {
        rb_buffer_set_max_len(
            c->buffer,
            rb_max(rb_buffer_get_length(c->buffer) * RB_BUFFER_MAX_LEN_FACTOR, (unsigned)RB_BUFFER_MAX_LEN_MIN));
    }
    if (likely(!rb_unsigned_mul_overflows(rb_buffer_get_length(c->buffer), RB_BUFFER_MAX_OPS_FACTOR))) {
        rb_buffer_set_max_ops(
            c->buffer,
            rb_max(rb_buffer_get_length(c->buffer) * RB_BUFFER_MAX_OPS_FACTOR, (unsigned)RB_BUFFER_MAX_OPS_MIN));
    }

    /* Save the original direction, we use it later. */
    c->target_direction = rb_buffer_get_direction(c->buffer);

    rb_buffer_clear_output(c->buffer);

    rb_ot_shape_initialize_masks(c);
    rb_set_unicode_props(c->buffer);
    rb_insert_dotted_circle(c->buffer, c->face);

    rb_form_clusters(c->buffer);

    rb_ensure_native_direction(c->buffer);

    rb_ot_complex_shaper_preprocess_text(c->plan->shaper, c->plan, c->buffer, c->face);

    rb_ot_substitute_pre(c);
    rb_ot_position(c);
    rb_ot_substitute_post(c);

    rb_propagate_flags(c->buffer);

    rb_buffer_set_direction(c->buffer, c->target_direction);

    rb_buffer_set_max_len(c->buffer, RB_BUFFER_MAX_LEN_DEFAULT);
    rb_buffer_set_max_ops(c->buffer, RB_BUFFER_MAX_OPS_DEFAULT);
}

void rb_ot_shape(rb_shape_plan_t *shape_plan,
                  rb_face_t *face,
                  rb_buffer_t *buffer,
                  const rb_feature_t *features,
                  unsigned int num_features)
{
    rb_ot_shape_context_t c = {&shape_plan->ot, face, buffer, features, num_features};
    rb_ot_shape_internal(&c);
}
