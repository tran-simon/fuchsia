// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod renderer;
pub use crate::renderer::{
    cell_size_from_cell_height, font_size_from_cell_size, renderable_layers, LayerContent, Offset,
    RenderableLayer, Renderer,
};
