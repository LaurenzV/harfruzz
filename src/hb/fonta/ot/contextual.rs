use crate::hb::ot_layout_gsubgpos::OT::hb_ot_apply_context_t;
use crate::hb::ot_layout_gsubgpos::{
    apply_lookup, match_backtrack, match_func_t, match_glyph, match_input, match_lookahead, Apply,
    WouldApply, WouldApplyContext,
};
use skrifa::raw::tables::layout::{
    ChainedSequenceContextFormat1, ChainedSequenceContextFormat2, ChainedSequenceContextFormat3,
};
use skrifa::raw::types::BigEndian;
use ttf_parser::{opentype_layout::SequenceLookupRecord, GlyphId};

impl WouldApply for ChainedSequenceContextFormat1<'_> {
    fn would_apply(&self, _ctx: &WouldApplyContext) -> bool {
        false
    }
}

impl Apply for ChainedSequenceContextFormat1<'_> {
    fn apply(&self, ctx: &mut hb_ot_apply_context_t) -> Option<()> {
        let glyph = skrifa::GlyphId::from(ctx.buffer.cur(0).as_glyph().0);
        let index = self.coverage().ok()?.get(glyph)? as usize;
        let set = self.chained_seq_rule_sets().get(index)?.ok()?;
        for rule in set.chained_seq_rules().iter().filter_map(|rule| rule.ok()) {
            let backtrack = rule.backtrack_sequence();
            let input = rule.input_sequence();
            let lookahead = rule.lookahead_sequence();
            if apply_chain_context(
                ctx,
                backtrack,
                input,
                lookahead,
                [&match_glyph; 3],
                rule.seq_lookup_records()
                    .iter()
                    .map(|rec| SequenceLookupRecord {
                        sequence_index: rec.sequence_index(),
                        lookup_list_index: rec.lookup_list_index(),
                    }),
            )
            .is_some()
            {
                return Some(());
            }
        }
        None
    }
}

impl WouldApply for ChainedSequenceContextFormat2<'_> {
    fn would_apply(&self, _ctx: &WouldApplyContext) -> bool {
        false
    }
}

/// Value represents glyph class.
fn match_class<'a>(
    class_def: &'a Option<skrifa::raw::tables::layout::ClassDef<'a>>,
) -> impl Fn(GlyphId, u16) -> bool + 'a {
    |glyph, value| {
        class_def
            .as_ref()
            .map(|class_def| class_def.get(skrifa::GlyphId16::new(glyph.0)) == value)
            .unwrap_or(false)
    }
}

impl Apply for ChainedSequenceContextFormat2<'_> {
    fn apply(&self, ctx: &mut hb_ot_apply_context_t) -> Option<()> {
        let backtrack_classes = self.backtrack_class_def().ok();
        let input_classes = self.input_class_def().ok();
        let lookahead_classes = self.lookahead_class_def().ok();
        let glyph = ctx.buffer.cur(0).as_skrifa_glyph16();
        self.coverage().ok()?.get(glyph)?;
        let index = input_classes.as_ref()?.get(glyph) as usize;
        let set = self.chained_class_seq_rule_sets().get(index)?.ok()?;
        for rule in set
            .chained_class_seq_rules()
            .iter()
            .filter_map(|rule| rule.ok())
        {
            let backtrack = rule.backtrack_sequence();
            let input = rule.input_sequence();
            let lookahead = rule.lookahead_sequence();
            if apply_chain_context(
                ctx,
                backtrack,
                input,
                lookahead,
                [
                    &match_class(&backtrack_classes),
                    &match_class(&input_classes),
                    &match_class(&lookahead_classes),
                ],
                rule.seq_lookup_records()
                    .iter()
                    .map(|rec| SequenceLookupRecord {
                        sequence_index: rec.sequence_index(),
                        lookup_list_index: rec.lookup_list_index(),
                    }),
            )
            .is_some()
            {
                return Some(());
            }
        }
        None
    }
}

impl WouldApply for ChainedSequenceContextFormat3<'_> {
    fn would_apply(&self, ctx: &WouldApplyContext) -> bool {
        let input_coverages = self.input_coverages();
        (!ctx.zero_context
            || (self.backtrack_coverage_offsets().len() == 0
                && self.lookahead_coverage_offsets().len() == 0))
            && (ctx.glyphs.len() == input_coverages.len() + 1
                && input_coverages.iter().enumerate().all(|(i, coverage)| {
                    coverage
                        .map(|cov| {
                            cov.get(skrifa::GlyphId::from(ctx.glyphs[i + 1].0))
                                .is_some()
                        })
                        .unwrap_or(false)
                }))
    }
}

impl Apply for ChainedSequenceContextFormat3<'_> {
    fn apply(&self, ctx: &mut hb_ot_apply_context_t) -> Option<()> {
        let glyph = skrifa::GlyphId::from(ctx.buffer.cur(0).as_glyph().0);

        let input_coverages = self.input_coverages();
        input_coverages.get(0).ok()?.get(glyph)?;

        let backtrack_coverages = self.backtrack_coverages();
        let lookahead_coverages = self.lookahead_coverages();

        let back = |glyph: GlyphId, index: u16| {
            backtrack_coverages
                .get(index as usize)
                .map(|cov| cov.get(skrifa::GlyphId::from(glyph.0)).is_some())
                .unwrap_or_default()
        };

        let ahead = |glyph: GlyphId, index: u16| {
            lookahead_coverages
                .get(index as usize)
                .map(|cov| cov.get(skrifa::GlyphId::from(glyph.0)).is_some())
                .unwrap_or_default()
        };

        let input = |glyph: GlyphId, index: u16| {
            input_coverages
                .get(index as usize + 1)
                .map(|cov| cov.get(skrifa::GlyphId::from(glyph.0)).is_some())
                .unwrap_or_default()
        };

        let mut end_index = ctx.buffer.idx;
        let mut match_end = 0;
        let mut match_positions = smallvec::SmallVec::from_elem(0, 4);

        let input_matches = match_input(
            ctx,
            input_coverages.len() as u16 - 1,
            &input,
            &mut match_end,
            &mut match_positions,
            None,
        );

        if input_matches {
            end_index = match_end;
        }

        if !(input_matches
            && match_lookahead(
                ctx,
                lookahead_coverages.len() as u16,
                &ahead,
                match_end,
                &mut end_index,
            ))
        {
            ctx.buffer
                .unsafe_to_concat(Some(ctx.buffer.idx), Some(end_index));
            return None;
        }

        let mut start_index = ctx.buffer.out_len;

        if !match_backtrack(
            ctx,
            backtrack_coverages.len() as u16,
            &back,
            &mut start_index,
        ) {
            ctx.buffer
                .unsafe_to_concat_from_outbuffer(Some(start_index), Some(end_index));
            return None;
        }

        ctx.buffer
            .unsafe_to_break_from_outbuffer(Some(start_index), Some(end_index));
        apply_lookup(
            ctx,
            input_coverages.len() - 1,
            &mut match_positions,
            match_end,
            self.seq_lookup_records()
                .iter()
                .map(|rec| SequenceLookupRecord {
                    sequence_index: rec.sequence_index(),
                    lookup_list_index: rec.lookup_list_index(),
                }),
        );

        Some(())
    }
}

trait ToU16: Copy {
    fn to_u16(self) -> u16;
}

impl ToU16 for BigEndian<skrifa::GlyphId16> {
    fn to_u16(self) -> u16 {
        self.get().to_u16()
    }
}

impl ToU16 for BigEndian<u16> {
    fn to_u16(self) -> u16 {
        self.get()
    }
}

fn apply_chain_context<T: ToU16>(
    ctx: &mut hb_ot_apply_context_t,
    backtrack: &[T],
    input: &[T],
    lookahead: &[T],
    match_funcs: [&match_func_t; 3],
    lookups: impl Iterator<Item = SequenceLookupRecord>,
) -> Option<()> {
    // NOTE: Whenever something in this method changes, we also need to
    // change it in the `apply` implementation for ChainedContextLookup.
    let f1 = |glyph, index| {
        let value = (*backtrack.get(index as usize).unwrap()).to_u16();
        match_funcs[0](glyph, value)
    };

    let f2 = |glyph, index| {
        let value = (*lookahead.get(index as usize).unwrap()).to_u16();
        match_funcs[2](glyph, value)
    };

    let f3 = |glyph, index| {
        let value = (*input.get(index as usize).unwrap()).to_u16();
        match_funcs[1](glyph, value)
    };

    let mut end_index = ctx.buffer.idx;
    let mut match_end = 0;
    let mut match_positions = smallvec::SmallVec::from_elem(0, 4);

    let input_matches = match_input(
        ctx,
        input.len() as u16,
        &f3,
        &mut match_end,
        &mut match_positions,
        None,
    );

    if input_matches {
        end_index = match_end;
    }

    if !(input_matches
        && match_lookahead(ctx, lookahead.len() as u16, &f2, match_end, &mut end_index))
    {
        ctx.buffer
            .unsafe_to_concat(Some(ctx.buffer.idx), Some(end_index));
        return None;
    }

    let mut start_index = ctx.buffer.out_len;

    if !match_backtrack(ctx, backtrack.len() as u16, &f1, &mut start_index) {
        ctx.buffer
            .unsafe_to_concat_from_outbuffer(Some(start_index), Some(end_index));
        return None;
    }

    ctx.buffer
        .unsafe_to_break_from_outbuffer(Some(start_index), Some(end_index));
    apply_lookup(
        ctx,
        usize::from(input.len()),
        &mut match_positions,
        match_end,
        lookups,
    );

    Some(())
}
