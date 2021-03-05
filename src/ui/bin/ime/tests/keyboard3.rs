// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![cfg(test)]

use {
    anyhow::{Context as _, Error},
    fidl_fuchsia_input as input,
    fidl_fuchsia_ui_input::{self as ui_input, ImeServiceProxy},
    fidl_fuchsia_ui_input3 as ui_input3, fidl_fuchsia_ui_views as ui_views,
    fuchsia_async::{self as fasync},
    fuchsia_component::client::connect_to_service,
    fuchsia_scenic as scenic,
    futures::{
        future,
        stream::{FusedStream, StreamExt},
    },
    matches::assert_matches,
};

fn create_key_down_event(key: input::Key, modifiers: ui_input3::Modifiers) -> ui_input3::KeyEvent {
    ui_input3::KeyEvent {
        key: Some(key),
        modifiers: Some(modifiers),
        type_: Some(ui_input3::KeyEventType::Pressed),
        ..ui_input3::KeyEvent::EMPTY
    }
}

fn create_key_up_event(key: input::Key, modifiers: ui_input3::Modifiers) -> ui_input3::KeyEvent {
    ui_input3::KeyEvent {
        key: Some(key),
        modifiers: Some(modifiers),
        type_: Some(ui_input3::KeyEventType::Released),
        ..ui_input3::KeyEvent::EMPTY
    }
}

async fn expect_key_event(
    listener: &mut ui_input3::KeyboardListenerRequestStream,
) -> ui_input3::KeyEvent {
    let listener_request = listener.next().await;
    if let Some(Ok(ui_input3::KeyboardListenerRequest::OnKeyEvent { event, responder, .. })) =
        listener_request
    {
        responder.send(ui_input3::KeyEventStatus::Handled).expect("responding from key listener");
        event
    } else {
        panic!("Expected key event, got {:?}", listener_request);
    }
}

async fn dispatch_and_expect_key_event(
    ime_service: &ImeServiceProxy,
    listener: &mut ui_input3::KeyboardListenerRequestStream,
    event: ui_input3::KeyEvent,
) -> Result<(), Error> {
    let (was_handled, event_result) =
        future::join(ime_service.dispatch_key3(event.clone()), expect_key_event(listener)).await;

    assert_eq!(was_handled?, true);
    assert_eq!(event_result.key, event.key);
    assert_eq!(event_result.type_, event.type_);
    Ok(())
}

async fn focus_and_expect_key_event(
    ime_service: &ImeServiceProxy,
    listener: &mut ui_input3::KeyboardListenerRequestStream,
    view_ref: &mut ui_views::ViewRef,
    event: ui_input3::KeyEvent,
) -> Result<(), Error> {
    let (event_result, focus_result) =
        future::join(expect_key_event(listener), ime_service.view_focus_changed(view_ref)).await;

    focus_result?;

    assert_eq!(event, event_result);
    Ok(())
}

async fn expect_key_and_modifiers(
    listener: &mut ui_input3::KeyboardListenerRequestStream,
    key: input::Key,
    modifiers: ui_input3::Modifiers,
) {
    let event = expect_key_event(listener).await;
    assert_eq!(event.key, Some(key));
    assert_eq!(event.modifiers, Some(modifiers));
}

#[fasync::run_singlethreaded(test)]
async fn test_disconnecting_keyboard_client_disconnects_listener() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["keyboard3_integration_test"])
        .expect("syslog init should not fail");

    let ime_service = connect_to_service::<ui_input::ImeServiceMarker>()
        .context("Failed to connect to IME Service")?;

    let keyboard_service_client = connect_to_service::<ui_input3::KeyboardMarker>()
        .context("Failed to connect to input3 Keyboard service")?;

    let keyboard_service_other_client = connect_to_service::<ui_input3::KeyboardMarker>()
        .context("Failed to establish another connection to input3 Keyboard service")?;

    // Create fake client.
    let (listener_client_end, mut listener) =
        fidl::endpoints::create_request_stream::<ui_input3::KeyboardListenerMarker>()?;
    let view_ref = scenic::ViewRefPair::new()?.view_ref;

    keyboard_service_client
        .add_listener(&mut scenic::duplicate_view_ref(&view_ref)?, listener_client_end)
        .await
        .expect("add_listener for first client");

    // Create another fake client.
    let (other_listener_client_end, mut other_listener) =
        fidl::endpoints::create_request_stream::<ui_input3::KeyboardListenerMarker>()?;
    let other_view_ref = scenic::ViewRefPair::new()?.view_ref;

    keyboard_service_other_client
        .add_listener(&mut scenic::duplicate_view_ref(&other_view_ref)?, other_listener_client_end)
        .await
        .expect("add_listener for another client");

    // Focus second client.
    ime_service.view_focus_changed(&mut scenic::duplicate_view_ref(&other_view_ref)?).await?;

    // Drop proxy, emulating first client disconnecting from it.
    std::mem::drop(keyboard_service_client);

    // Expect disconnected client key event listener to be disconnected as well.
    assert_matches!(listener.next().await, None);
    assert_matches!(listener.is_terminated(), true);

    // Ensure that the other client is still connected.
    let (key, modifiers) = (input::Key::A, ui_input3::Modifiers::CapsLock);
    let dispatched_event = create_key_down_event(key, modifiers);
    let (was_handled, _) = future::join(
        ime_service.dispatch_key3(dispatched_event),
        expect_key_and_modifiers(&mut other_listener, key, modifiers),
    )
    .await;

    assert_eq!(was_handled?, true);

    let dispatched_event = create_key_up_event(key, modifiers);
    let (was_handled, _) = future::join(
        ime_service.dispatch_key3(dispatched_event),
        expect_key_and_modifiers(&mut other_listener, key, modifiers),
    )
    .await;

    assert_eq!(was_handled?, true);
    Ok(())
}

#[fasync::run_singlethreaded(test)]
async fn test_sync_cancel() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["keyboard3_integration_test"])
        .expect("syslog init should not fail");

    let ime_service = connect_to_service::<ui_input::ImeServiceMarker>()
        .context("Failed to connect to IME Service")?;

    let keyboard_service_client = connect_to_service::<ui_input3::KeyboardMarker>()
        .context("Failed to connect to input3 Keyboard service")?;

    let keyboard_service_other_client = connect_to_service::<ui_input3::KeyboardMarker>()
        .context("Failed to establish another connection to input3 Keyboard service")?;

    // Create fake client.
    let (listener_client_end, mut listener) =
        fidl::endpoints::create_request_stream::<ui_input3::KeyboardListenerMarker>()?;
    let view_ref = scenic::ViewRefPair::new()?.view_ref;

    keyboard_service_client
        .add_listener(&mut scenic::duplicate_view_ref(&view_ref)?, listener_client_end)
        .await
        .expect("add_listener for first client");

    // Create another fake client.
    let (other_listener_client_end, mut other_listener) =
        fidl::endpoints::create_request_stream::<ui_input3::KeyboardListenerMarker>()?;
    let other_view_ref = scenic::ViewRefPair::new()?.view_ref;

    keyboard_service_other_client
        .add_listener(&mut scenic::duplicate_view_ref(&other_view_ref)?, other_listener_client_end)
        .await
        .expect("add_listener for another client");

    let key1 = input::Key::A;
    let event1_press = ui_input3::KeyEvent {
        key: Some(key1),
        type_: Some(ui_input3::KeyEventType::Pressed),
        ..ui_input3::KeyEvent::EMPTY
    };
    let event1_release = ui_input3::KeyEvent {
        key: Some(key1),
        type_: Some(ui_input3::KeyEventType::Released),
        ..ui_input3::KeyEvent::EMPTY
    };

    // Focus first client.
    ime_service.view_focus_changed(&mut scenic::duplicate_view_ref(&view_ref)?).await?;

    // Press the key.
    dispatch_and_expect_key_event(&ime_service, &mut listener, event1_press).await?;

    // Focus second client, expect sync event.
    focus_and_expect_key_event(
        &ime_service,
        &mut other_listener,
        &mut scenic::duplicate_view_ref(&other_view_ref)?,
        ui_input3::KeyEvent {
            key: Some(input::Key::A),
            type_: Some(ui_input3::KeyEventType::Sync),
            ..ui_input3::KeyEvent::EMPTY
        },
    )
    .await?;

    // Release the key.
    dispatch_and_expect_key_event(&ime_service, &mut other_listener, event1_release).await?;

    // Focus first client again, expect CANCEL for the first key.
    focus_and_expect_key_event(
        &ime_service,
        &mut listener,
        &mut scenic::duplicate_view_ref(&view_ref)?,
        ui_input3::KeyEvent {
            key: Some(input::Key::A),
            type_: Some(ui_input3::KeyEventType::Cancel),
            ..ui_input3::KeyEvent::EMPTY
        },
    )
    .await?;

    Ok(())
}
