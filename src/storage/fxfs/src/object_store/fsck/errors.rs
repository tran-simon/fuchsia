// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        lsm_tree::types::ItemRef,
        object_store::allocator::{AllocatorKey, AllocatorValue},
    },
    std::ops::Range,
};

#[derive(Clone, Debug)]
pub enum FsckError {
    Warning(FsckWarning),
    Fatal(FsckFatal),
}

impl FsckError {
    /// Translates an error to a human-readable string, intended for reporting errors to the user.
    /// For debugging, std::fmt::Debug is preferred.
    // TODO(jfsulliv): Localization
    pub fn to_string(&self) -> String {
        match self {
            FsckError::Warning(w) => format!("WARNING: {}", w.to_string()),
            FsckError::Fatal(f) => format!("ERROR: {}", f.to_string()),
        }
    }
    pub fn is_fatal(&self) -> bool {
        if let FsckError::Fatal(_) = self {
            true
        } else {
            false
        }
    }
}

#[derive(Clone, Debug)]
pub enum FsckWarning {
    // An object ID was found which had no parent.
    // Parameters are (store_id, object_id)
    // TODO(jfsulliv): Implement this check.
    OrphanedObject(u64, u64),
}

impl FsckWarning {
    fn to_string(&self) -> String {
        match self {
            FsckWarning::OrphanedObject(store_id, object_id) => {
                format!("Orphaned object {} was found in store {}", object_id, store_id)
            }
        }
    }
}

#[derive(Clone, Debug)]
#[allow(dead_code)]
pub struct Allocation {
    range: Range<u64>,
    ref_count: i64,
}

impl From<ItemRef<'_, AllocatorKey, AllocatorValue>> for Allocation {
    fn from(item: ItemRef<'_, AllocatorKey, AllocatorValue>) -> Self {
        Self { range: item.key.device_range.clone(), ref_count: item.value.delta }
    }
}

#[derive(Clone, Debug)]
pub enum FsckFatal {
    MalformedGraveyard,
    MalformedStore(u64),
    AllocationMismatch(Allocation, Allocation),
    AllocatedBytesMismatch(u64, u64),
    MissingAllocation(Allocation),
    ExtraAllocations(Vec<Allocation>),
    RefCountMismatch(u64, u64, u64),
    ObjectCountMismatch(u64, u64, u64),
}

impl FsckFatal {
    fn to_string(&self) -> String {
        match self {
            FsckFatal::MalformedGraveyard => {
                "Graveyard file is malformed; root store is inconsistent".to_string()
            }
            FsckFatal::MalformedStore(id) => {
                format!("Object store {} is malformed; root store is inconsistent", id)
            }
            FsckFatal::AllocationMismatch(expected, actual) => {
                format!("Expected allocation {:?} but found allocation {:?}", expected, actual)
            }
            FsckFatal::AllocatedBytesMismatch(expected, actual) => {
                format!("Expected {} bytes allocated, but found {} bytes", expected, actual)
            }
            FsckFatal::MissingAllocation(allocation) => {
                format!("Expected but didn't find allocation {:?}", allocation)
            }
            FsckFatal::ExtraAllocations(allocations) => {
                format!("Unexpected allocations {:?}", allocations)
            }
            FsckFatal::RefCountMismatch(oid, expected, actual) => {
                format!("Object {} had {} references, expected {}", oid, actual, expected)
            }
            FsckFatal::ObjectCountMismatch(store_id, expected, actual) => {
                format!("Store {} had {} objects, expected {}", store_id, actual, expected)
            }
        }
    }
}
