use crate::hb::set_digest::{hb_set_digest_ext, hb_set_digest_t};

use alloc::vec::Vec;
use core::ops::Range;
use skrifa::raw::{
    tables::{
        gpos::{
            CursivePosFormat1, Gpos, MarkBasePosFormat1, MarkLigPosFormat1, MarkMarkPosFormat1,
            PairPos, PairPosFormat1, PairPosFormat2, SinglePos, SinglePosFormat1, SinglePosFormat2,
        },
        gsub::{
            AlternateSubstFormat1, ExtensionSubstFormat1, Gsub, LigatureSubstFormat1,
            MultipleSubstFormat1, ReverseChainSingleSubstFormat1, SingleSubst, SingleSubstFormat1,
            SingleSubstFormat2,
        },
        layout::{
            ChainedSequenceContext, ChainedSequenceContextFormat1, ChainedSequenceContextFormat2,
            ChainedSequenceContextFormat3, CoverageTable, Lookup, LookupFlag, SequenceContext,
            SequenceContextFormat1, SequenceContextFormat2, SequenceContextFormat3,
        },
    },
    types::GlyphId,
    FontData, FontRead, Offset, ReadError,
};

pub trait LookupHost<'a> {
    fn lookup_count(&self) -> u16;
    fn lookup_data(&self, index: u16) -> Result<LookupData<'a>, ReadError>;
}

impl<'a> LookupHost<'a> for Gsub<'a> {
    fn lookup_count(&self) -> u16 {
        self.lookup_list()
            .map(|list| list.lookup_count())
            .unwrap_or_default()
    }

    fn lookup_data(&self, index: u16) -> Result<LookupData<'a>, ReadError> {
        let list = self.lookup_list()?;
        let offset = list
            .lookup_offsets()
            .get(index as usize)
            .ok_or(ReadError::OutOfBounds)?
            .get()
            .to_usize()
            + self.lookup_list_offset().to_usize();
        Ok(LookupData {
            offset,
            is_subst: true,
            table_data: self.offset_data(),
        })
    }
}

impl<'a> LookupHost<'a> for Gpos<'a> {
    fn lookup_count(&self) -> u16 {
        self.lookup_list()
            .map(|list| list.lookup_count())
            .unwrap_or_default()
    }

    fn lookup_data(&self, index: u16) -> Result<LookupData<'a>, ReadError> {
        let list = self.lookup_list()?;
        let offset = list
            .lookup_offsets()
            .get(index as usize)
            .ok_or(ReadError::OutOfBounds)?
            .get()
            .to_usize()
            + self.lookup_list_offset().to_usize();
        Ok(LookupData {
            offset,
            is_subst: false,
            table_data: self.offset_data(),
        })
    }
}

pub struct LookupData<'a> {
    /// Offset of the lookup from the base of the layout table.
    offset: usize,
    /// True if the lookup comes from GSUB.
    is_subst: bool,
    /// Data of the layout table.
    table_data: FontData<'a>,
}

/// Cache containing lookup and subtable information for a single GSUB or
/// GPOS table.
#[derive(Clone, Default)]
pub struct LookupCache {
    pub lookups: Vec<LookupInfo>,
    pub subtables: Vec<SubtableInfo>,
}

impl LookupCache {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn clear(&mut self) {
        self.lookups.clear();
        self.subtables.clear();
    }

    pub fn create_all<'a>(&mut self, host: &impl LookupHost<'a>) {
        self.clear();
        let count = host.lookup_count();
        self.lookups.resize(count as usize, Default::default());
        for i in 0..count {
            let _ = self.get_or_create(host, i);
        }
    }

    pub fn get(&self, index: u16) -> Option<&LookupInfo> {
        let entry = self.lookups.get(index as usize)?;
        match entry.state {
            LookupState::Ready => Some(&entry),
            _ => None,
        }
    }

    pub fn get_or_create<'a>(
        &mut self,
        cx: &impl LookupHost<'a>,
        index: u16,
    ) -> Result<&LookupInfo, ReadError> {
        let index = index as usize;
        if index >= self.lookups.len() {
            self.lookups.resize(index + 1, LookupInfo::default());
        }
        let entry = &mut self.lookups[index];
        if entry.state != LookupState::Vacant {
            return Ok(entry);
        }
        entry.state = LookupState::Error;
        let data = cx.lookup_data(index as u16)?;
        entry.is_subst = data.is_subst;
        let lookup_data = data
            .table_data
            .split_off(data.offset)
            .ok_or(ReadError::OutOfBounds)?;
        let lookup: Lookup<()> = Lookup::read(lookup_data)?;
        let kind = lookup.lookup_type();
        let lookup_flag = lookup.lookup_flag();
        entry.props = u32::from(lookup.lookup_flag().to_bits());
        if lookup_flag.to_bits() & LookupFlag::USE_MARK_FILTERING_SET.to_bits() != 0 {
            entry.props |= (lookup.mark_filtering_set().unwrap_or_default() as u32) << 16;
        }
        entry.is_rtl = lookup_flag.to_bits() & LookupFlag::RIGHT_TO_LEFT.to_bits() != 0;
        if data.is_subst {
            entry.is_reversed =
                is_reversed(data.table_data, &lookup, data.offset).unwrap_or_default();
        }
        entry.subtables_start = self
            .subtables
            .len()
            .try_into()
            .map_err(|_| ReadError::MalformedData("too many subtables"))?;
        entry.state = LookupState::Ready;
        let mut process_subtable = |mut subtable_offset: usize| {
            let mut subtable_kind = kind;
            match (data.is_subst, kind) {
                (true, 7) | (false, 9) => {
                    let subtable_data = data
                        .table_data
                        .split_off(subtable_offset)
                        .ok_or(ReadError::OutOfBounds)?;
                    let ext = ExtensionSubstFormat1::<()>::read(subtable_data)?;
                    subtable_kind = ext.extension_lookup_type();
                    let ext_offset = ext.extension_offset().to_usize();
                    subtable_offset += ext_offset;
                }
                _ => {}
            }
            let mut subtable_info = SubtableInfo {
                offset: subtable_offset
                    .try_into()
                    .map_err(|_| ReadError::OutOfBounds)?,
                coverage_offset: 0,
                is_subst: data.is_subst,
                lookup_type: subtable_kind as u8,
                digest: Default::default(),
            };
            // TODO: update as we add more subtables
            let is_supported = match (data.is_subst, subtable_kind) {
                // (true, 1) | (true, 2) | (true, 3) | (true, 4) => true,
                (true, 1) | (true, 2) | (true, 3) | (true, 4) => true,
                (false, 4) | (false, 6) => true,
                // single pos
                (false, 1) => true,
                // pair pos
                (false, 2) => true,
                // cursive pos
                (false, 3) => true,
                // mark lig pos
                (false, 5) => true,
                // chained sequence context
                (true, 6) => true,
                (false, 8) => true,
                // reverse chained context
                (true, 8) => true,
                _ => false,
            };
            if !is_supported {
                return Err(ReadError::MalformedData("unsupported subtable"));
            }
            let subtable = subtable_info.materialize(data.table_data.as_bytes())?;
            let (coverage, coverage_offset) = subtable.coverage_and_offset()?;
            add_coverage_to_digest(&coverage, &mut subtable_info.digest);
            add_coverage_to_digest(&coverage, &mut entry.digest);
            // subtable_info.digest.insert_coverage(&coverage);
            // entry.digest.insert_coverage(&coverage);
            subtable_info.coverage_offset = coverage_offset;
            self.subtables.push(subtable_info);
            entry.subtables_count += 1;
            Ok::<(), ReadError>(())
        };
        for subtable_offset in lookup.subtable_offsets() {
            let subtable_offset = subtable_offset.get().to_usize() + data.offset;
            // Just drop subtables with errors
            let _ = process_subtable(subtable_offset);
        }
        Ok(entry)
    }

    pub fn subtables(&self, entry: &LookupInfo) -> Option<&[SubtableInfo]> {
        self.subtables.get(entry.subtables_range())
    }
}

fn is_reversed(table_data: FontData, lookup: &Lookup<()>, lookup_offset: usize) -> Option<bool> {
    match lookup.lookup_type() {
        // Reverse chain context
        8 => Some(true),
        // Extension table
        7 => {
            let offset = lookup_offset + lookup.subtable_offsets().first()?.get().to_usize();
            let data = table_data.split_off(offset)?;
            let ext = ExtensionSubstFormat1::<()>::read(data).ok()?;
            Some(ext.extension_lookup_type() == 8)
        }
        _ => Some(false),
    }
}

fn add_coverage_to_digest(coverage: &CoverageTable, digest: &mut hb_set_digest_t) {
    match coverage {
        CoverageTable::Format1(table) => {
            for glyph in table.glyph_array() {
                digest.add(ttf_parser::GlyphId(glyph.get().to_u32() as _));
            }
        }
        CoverageTable::Format2(table) => {
            for range in table.range_records() {
                let first = range.start_glyph_id().to_u32();
                let last = range.end_glyph_id().to_u32();
                let [first, last] = [first, last].map(|gid| ttf_parser::GlyphId(gid as _));
                digest.add_range(first, last);
            }
        }
    }
}

/// Current state of a lookup cache entry.
#[derive(Copy, Clone, PartialEq, Eq, Default, Debug)]
#[repr(u8)]
pub enum LookupState {
    /// Lookup has not been cached yet. This supports
    /// lazy population of the lookup cache.
    #[default]
    Vacant,
    /// Lookup is available for use.
    Ready,
    /// An error occured while reading this lookup.
    Error,
}

/// Cached information about a lookup.
#[derive(Clone, Default, Debug)]
pub struct LookupInfo {
    /// Current state of this lookup info entry.
    pub state: LookupState,
    pub props: u32,
    pub is_subst: bool,
    /// Indicates RTL processing for cursive lookups.
    pub is_rtl: bool,
    /// True if glyphs should be processed in reverse for this lookup.
    pub is_reversed: bool,
    /// Index of the first subtable in the cache subtables vector.
    pub subtables_start: u32,
    /// Number of subtables in the cache subtables vector.
    pub subtables_count: u16,
    /// Bloom filter representing the set of glyphs from the primary
    /// coverage of all subtables in the lookup.
    pub digest: hb_set_digest_t,
}

impl LookupInfo {
    pub fn subtables_range(&self) -> Range<usize> {
        let start = self.subtables_start as usize;
        start..start + self.subtables_count as usize
    }
}

/// Cached information about a subtable.
#[derive(Clone, Debug)]
pub struct SubtableInfo {
    /// Byte offset to the subtable from the base of the GSUB or GPOS
    /// table.
    pub offset: u32,
    /// Byte offset to the primary coverage table from the base of
    /// the subtable.
    pub coverage_offset: u16,
    /// Indicates whether the subtable is part of GSUB or GPOS for
    /// differentiating the lookup type.
    pub is_subst: bool,
    /// Original lookup type.
    pub lookup_type: u8,
    pub digest: hb_set_digest_t,
}

impl SubtableInfo {
    pub fn primary_coverage_table<'a>(
        &self,
        table_data: &'a [u8],
    ) -> Result<CoverageTable<'a>, ReadError> {
        let offset = self.offset as usize + self.coverage_offset as usize;
        let data = FontData::new(table_data.get(offset..).ok_or(ReadError::OutOfBounds)?);
        CoverageTable::read(data)
    }

    pub fn primary_coverage(&self, table_data: &[u8], glyph_id: GlyphId) -> Option<u16> {
        let coverage = self.primary_coverage_table(table_data).ok()?;
        coverage.get(glyph_id)
    }

    pub fn materialize<'a>(&self, table_data: &'a [u8]) -> Result<Subtable<'a>, ReadError> {
        let data = FontData::new(
            table_data
                .get(self.offset as usize..)
                .ok_or(ReadError::OutOfBounds)?,
        );
        Subtable::read(data, self.is_subst, self.lookup_type)
    }
}

/// All possible subtables in a lookup.
#[derive(Clone)]
pub enum Subtable<'a> {
    SingleSubst1(SingleSubstFormat1<'a>),
    SingleSubst2(SingleSubstFormat2<'a>),
    MultipleSubst1(MultipleSubstFormat1<'a>),
    AlternateSubst1(AlternateSubstFormat1<'a>),
    LigatureSubst1(LigatureSubstFormat1<'a>),
    SinglePos1(SinglePosFormat1<'a>),
    SinglePos2(SinglePosFormat2<'a>),
    PairPos1(PairPosFormat1<'a>),
    PairPos2(PairPosFormat2<'a>),
    CursivePos1(CursivePosFormat1<'a>),
    MarkBasePos1(MarkBasePosFormat1<'a>),
    MarkMarkPos1(MarkMarkPosFormat1<'a>),
    MarkLigPos1(MarkLigPosFormat1<'a>),
    ContextFormat1(SequenceContextFormat1<'a>),
    ContextFormat2(SequenceContextFormat2<'a>),
    ContextFormat3(SequenceContextFormat3<'a>),
    ChainedContextFormat1(ChainedSequenceContextFormat1<'a>),
    ChainedContextFormat2(ChainedSequenceContextFormat2<'a>),
    ChainedContextFormat3(ChainedSequenceContextFormat3<'a>),
    ReverseChainContext(ReverseChainSingleSubstFormat1<'a>),
}

impl<'a> Subtable<'a> {
    fn read(data: FontData<'a>, is_sub: bool, lookup_type: u8) -> Result<Self, ReadError> {
        match (is_sub, lookup_type) {
            (true, 1) => match SingleSubst::read(data)? {
                SingleSubst::Format1(s) => Ok(Self::SingleSubst1(s)),
                SingleSubst::Format2(s) => Ok(Self::SingleSubst2(s)),
            },
            (false, 1) => match SinglePos::read(data)? {
                SinglePos::Format1(s) => Ok(Self::SinglePos1(s)),
                SinglePos::Format2(s) => Ok(Self::SinglePos2(s)),
            },
            (true, 2) => Ok(Self::MultipleSubst1(MultipleSubstFormat1::read(data)?)),
            (false, 2) => match PairPos::read(data)? {
                PairPos::Format1(s) => Ok(Self::PairPos1(s)),
                PairPos::Format2(s) => Ok(Self::PairPos2(s)),
            },
            (true, 3) => Ok(Self::AlternateSubst1(AlternateSubstFormat1::read(data)?)),
            (false, 3) => Ok(Self::CursivePos1(CursivePosFormat1::read(data)?)),
            (true, 4) => Ok(Self::LigatureSubst1(LigatureSubstFormat1::read(data)?)),
            (false, 4) => Ok(Self::MarkBasePos1(MarkBasePosFormat1::read(data)?)),
            (true, 5) | (false, 7) => match SequenceContext::read(data)? {
                SequenceContext::Format1(s) => Ok(Self::ContextFormat1(s)),
                SequenceContext::Format2(s) => Ok(Self::ContextFormat2(s)),
                SequenceContext::Format3(s) => Ok(Self::ContextFormat3(s)),
            },
            (false, 5) => Ok(Self::MarkLigPos1(MarkLigPosFormat1::read(data)?)),
            (true, 6) | (false, 8) => match ChainedSequenceContext::read(data)? {
                ChainedSequenceContext::Format1(s) => Ok(Self::ChainedContextFormat1(s)),
                ChainedSequenceContext::Format2(s) => Ok(Self::ChainedContextFormat2(s)),
                ChainedSequenceContext::Format3(s) => Ok(Self::ChainedContextFormat3(s)),
            },
            (false, 6) => Ok(Self::MarkMarkPos1(MarkMarkPosFormat1::read(data)?)),
            (true, 7) | (false, 9) => {
                let ext = ExtensionSubstFormat1::<'a, ()>::read(data)?;
                let ext_type = ext.extension_lookup_type() as u8;
                let offset = ext.extension_offset().to_usize();
                let data = data.split_off(offset).ok_or(ReadError::OutOfBounds)?;
                Self::read(data, is_sub, ext_type)
            }
            (true, 8) => Ok(Self::ReverseChainContext(
                ReverseChainSingleSubstFormat1::read(data)?,
            )),
            _ => Err(ReadError::MalformedData("invalid lookup type")),
        }
    }

    fn coverage_and_offset(&self) -> Result<(CoverageTable<'a>, u16), ReadError> {
        match self {
            Self::SingleSubst1(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::SingleSubst2(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::MultipleSubst1(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::AlternateSubst1(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::LigatureSubst1(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::ReverseChainContext(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::SinglePos1(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::SinglePos2(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::PairPos1(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::PairPos2(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::CursivePos1(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::MarkBasePos1(s) => {
                Ok((s.mark_coverage()?, s.mark_coverage_offset().to_u32() as _))
            }
            Self::MarkMarkPos1(s) => {
                Ok((s.mark1_coverage()?, s.mark1_coverage_offset().to_u32() as _))
            }
            Self::MarkLigPos1(s) => {
                Ok((s.mark_coverage()?, s.mark_coverage_offset().to_u32() as _))
            }
            Self::ContextFormat1(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::ContextFormat2(s) => Ok((s.coverage()?, s.coverage_offset().to_u32() as _)),
            Self::ContextFormat3(s) => {
                let offset = s.coverage_offsets().first().ok_or(ReadError::OutOfBounds)?;
                Ok((s.coverages().get(0)?, offset.get().to_u32() as _))
            }
            Self::ChainedContextFormat1(s) => {
                Ok((s.coverage()?, s.coverage_offset().to_u32() as _))
            }
            Self::ChainedContextFormat2(s) => {
                Ok((s.coverage()?, s.coverage_offset().to_u32() as _))
            }
            Self::ChainedContextFormat3(s) => {
                let offset = s
                    .input_coverage_offsets()
                    .first()
                    .ok_or(ReadError::OutOfBounds)?;
                Ok((s.input_coverages().get(0)?, offset.get().to_u32() as _))
            }
        }
    }
}
