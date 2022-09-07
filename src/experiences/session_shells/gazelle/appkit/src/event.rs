// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl::endpoints::{ClientEnd, ServerEnd},
    fidl_fuchsia_element as felement, fidl_fuchsia_ui_input3 as ui_input3,
    fidl_fuchsia_ui_views as ui_views,
};

use crate::{child_view::ChildViewId, window::WindowId};

/// Defines a set of events generated by the library.
#[derive(Debug)]
pub enum Event<T> {
    /// Use Init to perform one-time app initialization.
    Init,
    /// Set of system level events that are window agnostic.
    SystemEvent(SystemEvent),
    /// Set of device level events that are window agnostic.
    DeviceEvent,
    /// Set of events that apply to a window instance.
    WindowEvent(WindowId, WindowEvent),
    /// Set of events that apply to embedded child views.
    ChildViewEvent(ChildViewId, WindowId, ChildViewEvent),
    /// Used to route application specific events T.
    UserEvent(T),
    /// Use to notify the event processing loop to terminate.
    Exit,
}

/// Set of system level events that are window agnostic.
#[derive(Debug)]
pub enum SystemEvent {
    /// Can be used to create a window from a [ui_views::ViewCreationToken]. This is useful only
    /// for ViewProvider based applications.
    ViewCreationToken(ui_views::ViewCreationToken),
    /// Used for creating a child view given a ViewSpecHolder using [window.create_child_view].
    PresentViewSpec(ViewSpecHolder),
}

/// The next future presentation time, expressed in nanoseconds in the `CLOCK_MONOTONIC` timebase.
type NextPresentTimeInNanos = i64;

/// Set of events that apply to a window instance.
#[derive(Debug)]
pub enum WindowEvent {
    /// Window is resized. This is also the first event sent upon window creation.
    Resized(u32, u32),
    /// Window needs to be redrawn. Sent upon receiving the next frame request from Flatland.
    NeedsRedraw(NextPresentTimeInNanos),
    /// Window has received or lost focus.
    Focused(bool),
    /// A keyboard key event when the window is in focus.
    Keyboard(ui_input3::KeyEvent, ui_input3::KeyboardListenerOnKeyEventResponder),
    /// Window was closed by the [GraphicalPresenter] presenting this window.
    Closed,
}

/// Set of events that apply to embedded child views.
#[derive(Debug)]
pub enum ChildViewEvent {
    /// Child view is created but not attached to the view tree yet.
    Available,
    /// Child view is attached to the view tree.
    Attached(ui_views::ViewRef),
    /// Child view is detached from the view tree.
    Detached,
}

/// Defines a struct to hold the parameters provided during [GraphicalPresenter.present_view]. This
/// is used when the applications implements the [GraphicalPresenter] protocol to embed child views.
#[derive(Debug)]
pub struct ViewSpecHolder {
    pub view_spec: felement::ViewSpec,
    pub annotation_controller: Option<ClientEnd<felement::AnnotationControllerMarker>>,
    pub view_controller_request: Option<ServerEnd<felement::ViewControllerMarker>>,
    pub responder: felement::GraphicalPresenterPresentViewResponder,
}
