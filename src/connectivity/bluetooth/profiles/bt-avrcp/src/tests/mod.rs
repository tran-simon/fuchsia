// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    assert_matches::assert_matches,
    async_utils::PollExt,
    bt_avctp::{AvcCommand, AvcPeer, AvcResponseType, AvctpCommand, AvctpCommandStream, AvctpPeer},
    fidl::endpoints::{create_endpoints, create_proxy, create_proxy_and_stream},
    fidl_fuchsia_bluetooth_avrcp::{self as fidl_avrcp, *},
    fidl_fuchsia_bluetooth_avrcp_test::*,
    fidl_fuchsia_bluetooth_bredr::{ProfileMarker, ProfileRequestStream},
    fuchsia_async as fasync,
    fuchsia_bluetooth::{
        profile::Psm,
        types::{Channel, PeerId},
    },
    fuchsia_zircon as zx,
    futures::{
        channel::mpsc, future::FutureExt, stream::StreamExt, stream::TryStreamExt, task::Poll,
    },
    packet_encoding::{Decodable, Encodable},
    pin_utils::pin_mut,
    std::collections::HashSet,
    std::convert::TryFrom,
};

use crate::{
    browse_controller_service, controller_service,
    packets::{
        player_application_settings::PlayerApplicationSettingAttributeId, Error as PacketError,
        GetCapabilitiesCapabilityId, PlaybackStatus, *,
    },
    peer::BrowseChannelHandler,
    peer_manager::PeerManager,
    peer_manager::ServiceRequest,
    profile::{AvrcpProtocolVersion, AvrcpService, AvrcpTargetFeatures},
    service,
};

fn spawn_peer_manager() -> (
    PeerManager,
    ProfileRequestStream,
    PeerManagerProxy,
    PeerManagerExtProxy,
    mpsc::Receiver<ServiceRequest>,
) {
    let (client_sender, service_request_receiver) = mpsc::channel(512);

    let (profile_proxy, profile_requests) =
        create_proxy_and_stream::<ProfileMarker>().expect("should have initialized");

    let peer_manager = PeerManager::new(profile_proxy);

    let (peer_manager_proxy, peer_manager_requests) =
        create_proxy_and_stream::<PeerManagerMarker>().expect("should have initialized");

    let (ext_proxy, ext_requests) =
        create_proxy_and_stream::<PeerManagerExtMarker>().expect("should have initialized");

    let sender = client_sender.clone();
    fasync::Task::spawn(async move {
        let _ = service::handle_peer_manager_requests(
            peer_manager_requests,
            sender,
            &controller_service::spawn_service,
            &browse_controller_service::spawn_service,
        )
        .await;
    })
    .detach();

    let sender = client_sender.clone();
    fasync::Task::spawn(async move {
        let _ = service::handle_peer_manager_ext_requests(
            ext_requests,
            sender,
            &controller_service::spawn_ext_service,
            &browse_controller_service::spawn_ext_service,
        )
        .await;
    })
    .detach();

    (peer_manager, profile_requests, peer_manager_proxy, ext_proxy, service_request_receiver)
}

#[test]
fn target_delegate_target_handler_already_bound_test() -> Result<(), Error> {
    let mut exec = fasync::TestExecutor::new().expect("executor should create");

    let (
        mut peer_manager,
        _profile_requests,
        peer_manager_proxy,
        _ext_proxy,
        mut service_request_receiver,
    ) = spawn_peer_manager();

    // create an target handler.
    let (target_client, target_server) = create_endpoints::<TargetHandlerMarker>()?;

    // Make a request and start it.  It should be pending.
    let register_fut = peer_manager_proxy.register_target_handler(target_client);
    pin_mut!(register_fut);

    assert!(exec.run_until_stalled(&mut register_fut).is_pending());

    // We should have a service request.
    let request = service_request_receiver.try_next()?.expect("S&ervice request should be handled");

    // Handing it to Peer Manager should resolve the request
    peer_manager.handle_service_request(request);

    assert_matches!(exec.run_until_stalled(&mut register_fut), Poll::Ready(Ok(Ok(()))));

    // Dropping the server end of this should drop the handler.
    drop(target_server);

    // create an new target handler.
    let (target_client_2, _target_server_2) = create_endpoints::<TargetHandlerMarker>()?;

    // should succeed if the previous handler was dropped.
    let register_fut = peer_manager_proxy.register_target_handler(target_client_2);
    pin_mut!(register_fut);
    assert!(exec.run_until_stalled(&mut register_fut).is_pending());
    let request = service_request_receiver.try_next()?.expect("Service request should be handled");
    peer_manager.handle_service_request(request);
    assert_matches!(exec.run_until_stalled(&mut register_fut), Poll::Ready(Ok(Ok(()))));

    // create an new target handler.
    let (target_client_3, _target_server_3) = create_endpoints::<TargetHandlerMarker>()?;

    // should fail since the target handler is already set.
    let register_fut = peer_manager_proxy.register_target_handler(target_client_3);
    pin_mut!(register_fut);
    assert!(exec.run_until_stalled(&mut register_fut).is_pending());
    let request = service_request_receiver.try_next()?.expect("Service request should be handled");
    peer_manager.handle_service_request(request);
    let expected_status = zx::Status::ALREADY_BOUND.into_raw();
    match exec.run_until_stalled(&mut register_fut) {
        Poll::Ready(Ok(Err(status))) => assert_eq!(status, expected_status),
        x => panic!("Expected a ready error result, got {:?}", x),
    }
    Ok(())
}

/// Helper function for decoding AVCTP command that the remote peer received.
/// It checks that the PduId of the message is equal to the expected PduId.
#[track_caller]
fn decode_avctp_command(command: &AvctpCommand, expected_pdu_id: PduId) -> Vec<u8> {
    // Decode the provided `command` into a PduId and command parameters.
    match BrowseChannelHandler::decode_command(command) {
        Ok((id, packet)) if id == expected_pdu_id => packet,
        result => panic!("[Browse Channel] Received unexpected result: {:?}", result),
    }
}

/// Helper function to send AVCTP response back as a remote peer.
fn send_avctp_response(
    pdu_id: PduId,
    response: &impl Encodable<Error = PacketError>,
    command: &AvctpCommand,
) {
    let mut buf = vec![0; response.encoded_len()];
    response.encode(&mut buf[..]).expect("should have succeeded");

    // Send the response back to the remote peer.
    let response_packet = BrowsePreamble::new(u8::from(&pdu_id), buf);
    let mut response_buf = vec![0; response_packet.encoded_len()];
    response_packet.encode(&mut response_buf[..]).expect("Encoding should work");

    let _ = command.send_response(&response_buf[..]).expect("should have succeeded");
}

#[test]
fn target_delegate_volume_handler_already_bound_test() -> Result<(), Error> {
    let mut exec = fasync::TestExecutor::new().expect("executor should create");

    let (
        mut peer_manager,
        _profile_requests,
        peer_manager_proxy,
        _ext_proxy,
        mut service_request_receiver,
    ) = spawn_peer_manager();

    // create a volume handler.
    let (volume_client, volume_server) = create_endpoints::<AbsoluteVolumeHandlerMarker>()?;

    // Make a request and start it.  It should be pending.
    let register_fut = peer_manager_proxy.set_absolute_volume_handler(volume_client);
    pin_mut!(register_fut);

    assert!(exec.run_until_stalled(&mut register_fut).is_pending());

    // We should have a service request.
    let request = service_request_receiver.try_next()?.expect("Service request should be handled");

    // Handing it to Peer Manager should resolve the request
    peer_manager.handle_service_request(request);

    assert_matches!(exec.run_until_stalled(&mut register_fut), Poll::Ready(Ok(Ok(()))));

    // Dropping the server end of this should drop the handler.
    drop(volume_server);

    // create an new target handler.
    let (volume_client_2, _volume_server_2) = create_endpoints::<AbsoluteVolumeHandlerMarker>()?;

    // should succeed if the previous handler was dropped.
    let register_fut = peer_manager_proxy.set_absolute_volume_handler(volume_client_2);
    pin_mut!(register_fut);
    assert!(exec.run_until_stalled(&mut register_fut).is_pending());
    let request = service_request_receiver.try_next()?.expect("Service request should be handled");
    peer_manager.handle_service_request(request);
    assert_matches!(exec.run_until_stalled(&mut register_fut), Poll::Ready(Ok(Ok(()))));

    // create an new target handler.
    let (volume_client_3, _volume_server_3) = create_endpoints::<AbsoluteVolumeHandlerMarker>()?;

    // should fail since the target handler is already set.
    let register_fut = peer_manager_proxy.set_absolute_volume_handler(volume_client_3);
    pin_mut!(register_fut);
    assert!(exec.run_until_stalled(&mut register_fut).is_pending());
    let request = service_request_receiver.try_next()?.expect("Service request should be handled");
    peer_manager.handle_service_request(request);
    let expected_status = zx::Status::ALREADY_BOUND.into_raw();
    match exec.run_until_stalled(&mut register_fut) {
        Poll::Ready(Ok(Err(status))) => assert_eq!(status, expected_status),
        x => panic!("Expected a ready error result, got {:?}", x),
    }
    Ok(())
}

/// Integration test of the peer manager and the FIDL front end with a mock BDEDR backend an
/// emulated remote peer. Validates we can get a controller to a device we discovered, we can send
/// commands on that controller, and that we can send responses and have them be dispatched back as
/// responses to the FIDL frontend in AVRCP. Exercises a majority of the peer manager
/// controller logic.
/// 1. Creates a front end FIDL endpoints for the test controller interface, a peer manager, and mock
///    backend.
/// 2. It then creates a channel and injects a fake services discovered and incoming connection
///    event into the mock profile service.
/// 3. Obtains both a regular and test controller from the FIDL service.
/// 4. Issues a Key0 passthrough keypress and validates we got both a key up and key down event
/// 4. Issues a GetCapabilities command using get_events_supported on the test controller FIDL
/// 5. Issues a SetAbsoluteVolume command on the controller FIDL
/// 6. Issues a GetElementAttributes command, encodes multiple packets, and handles continuations
/// 7. Issues a GetPlayStatus command on the controller FIDL.
/// 8. Issues a GetPlayerApplicationSettings command on the controller FIDL.
/// 9. Issues a SetPlayerApplicationSettings command on the controller FIDL.
/// 10. Register event notification for position change callbacks and mock responses.
/// 11. Waits until we have a response to all commands from our mock remote service return expected
///    values and we have received enough position change events.
#[fuchsia::test]
async fn test_peer_manager_with_fidl_client_and_mock_profile() -> Result<(), Error> {
    const REQUESTED_VOLUME: u8 = 0x40;
    const SET_VOLUME: u8 = 0x24;
    let fake_peer_id = PeerId(0);
    const LOREM_IPSUM: &str = "Lorem ipsum dolor sit amet,\
         consectetur adipiscing elit. Nunc eget elit cursus ipsum \
         fermentum viverra id vitae lorem. Cras luctus elementum \
         metus vel volutpat. Vestibulum ante ipsum primis in \
         faucibus orci luctus et ultrices posuere cubilia \
         Curae; Praesent efficitur velit sed metus luctus \
         Mauris in ante ultrices, vehicula lorem non, sagittis metus. \
         Nam facilisis volutpat quam. Suspendisse sem ipsum, blandit ut faucibus vitae,\
         facilisis quis massa. Aliquam sagittis, orci sed dignissim vulputate, odio neque \
         tempor dui, vel feugiat metus massa id urna. Nam at risus sem.\
         Duis commodo suscipit metus, at placerat elit suscipit eget. Suspendisse interdum \
         id metus vitae porta. Ut cursus viverra imperdiet. Aliquam erat volutpat. \
         Curabitur vehicula mauris nec ex sollicitudin rhoncus. Integer ipsum libero, \
         porta id velit et, egestas facilisis tellus.\
         Mauris in ante ultrices, vehicula lorem non, sagittis metus.\
         Nam facilisis volutpat quam. Suspendisse sem ipsum, blandit ut faucibus vitae,\
         facilisis quis massa. Aliquam sagittis, orci sed dignissim vulputate, odio neque \
         tempor dui, vel feugiat metus massa id urna. Nam at risus sem.\
         Duis commodo suscipit metus, at placerat elit suscipit eget. Suspendisse interdum \
         id metus vitae porta. Ut cursus viverra imperdiet. Aliquam erat volutpat. \
         Curabitur vehicula mauris nec ex sollicitudin rhoncus. Integer ipsum libero, \
         porta id velit et, egestas facilisis tellus.";

    // when zero, we exit the test.
    let mut expected_commands: i64 = 0;

    let (peer_manager_proxy, peer_manager_requests) =
        create_proxy_and_stream::<PeerManagerMarker>()?;
    let (ext_proxy, ext_requests) = create_proxy_and_stream::<PeerManagerExtMarker>()?;

    let (client_sender, mut peer_controller_request_receiver) = mpsc::channel(512);

    let (local, remote) = Channel::create();
    let remote_peer = AvcPeer::new(remote);
    let (profile_proxy, _requests) = create_proxy::<ProfileMarker>()?;

    let mut peer_manager = PeerManager::new(profile_proxy);

    peer_manager.services_found(
        &fake_peer_id,
        vec![AvrcpService::Target {
            features: AvrcpTargetFeatures::CATEGORY1,
            psm: Psm::AVCTP,
            protocol_version: AvrcpProtocolVersion(1, 6),
        }],
    );

    peer_manager.new_control_connection(&fake_peer_id, local);

    let handler_fut = service::handle_peer_manager_requests(
        peer_manager_requests,
        client_sender.clone(),
        &controller_service::spawn_service,
        &browse_controller_service::spawn_service,
    )
    .fuse();
    pin_mut!(handler_fut);

    let test_handler_fut = service::handle_peer_manager_ext_requests(
        ext_requests,
        client_sender.clone(),
        &controller_service::spawn_ext_service,
        &browse_controller_service::spawn_ext_service,
    )
    .fuse();
    pin_mut!(test_handler_fut);

    let (controller_proxy, controller_server) = create_proxy()?;
    let get_controller_fut = peer_manager_proxy
        .get_controller_for_target(&mut fake_peer_id.into(), controller_server)
        .fuse();
    pin_mut!(get_controller_fut);

    let (controller_ext_proxy, controller_ext_server) = create_proxy()?;
    let get_test_controller_fut =
        ext_proxy.get_controller_for_target(&mut fake_peer_id.into(), controller_ext_server).fuse();
    pin_mut!(get_test_controller_fut);

    let is_connected_fut = controller_ext_proxy.is_connected().fuse();
    pin_mut!(is_connected_fut);

    let passthrough_fut = controller_proxy.send_command(AvcPanelCommand::Key0).fuse();
    pin_mut!(passthrough_fut);
    expected_commands += 1;
    let mut keydown_pressed = false;
    let mut keyup_pressed = false;

    let volume_fut = controller_proxy.set_absolute_volume(REQUESTED_VOLUME).fuse();
    pin_mut!(volume_fut);
    expected_commands += 1;

    let events_fut = controller_ext_proxy.get_events_supported().fuse();
    pin_mut!(events_fut);
    expected_commands += 1;

    let get_media_attributes_fut = controller_proxy.get_media_attributes().fuse();
    pin_mut!(get_media_attributes_fut);
    expected_commands += 1;

    let get_play_status_fut = controller_proxy.get_play_status().fuse();
    pin_mut!(get_play_status_fut);
    expected_commands += 1;

    let attribute_ids = vec![fidl_avrcp::PlayerApplicationSettingAttributeId::Equalizer];
    let get_player_application_settings_fut =
        controller_proxy.get_player_application_settings(&mut attribute_ids.into_iter()).fuse();
    pin_mut!(get_player_application_settings_fut);
    expected_commands += 1;

    let attribute_ids_empty = vec![];
    let get_all_player_application_settings_fut = controller_proxy
        .get_player_application_settings(&mut attribute_ids_empty.into_iter())
        .fuse();
    pin_mut!(get_all_player_application_settings_fut);
    expected_commands += 1;

    let mut settings = fidl_avrcp::PlayerApplicationSettings::EMPTY;
    settings.scan_mode = Some(fidl_avrcp::ScanMode::GroupScan);
    settings.shuffle_mode = Some(fidl_avrcp::ShuffleMode::Off);
    let set_player_application_settings_fut =
        controller_proxy.set_player_application_settings(settings).fuse();
    pin_mut!(set_player_application_settings_fut);
    expected_commands += 1;

    let current_battery_status = BatteryStatus::Warning;
    let inform_battery_status_fut =
        controller_proxy.inform_battery_status(current_battery_status).fuse();
    pin_mut!(inform_battery_status_fut);
    expected_commands += 1;

    let mut additional_packets: Vec<Vec<u8>> = vec![];

    // set controller event filter to ones we support.
    let _ = controller_proxy
        .set_notification_filter(Notifications::TRACK_POS | Notifications::VOLUME, 1)?;

    let mut volume_value_received = false;

    let event_stream = controller_proxy.take_event_stream();
    pin_mut!(event_stream);

    let mut position_changed_events = 0;

    let remote_command_stream = remote_peer.take_command_stream().fuse();
    pin_mut!(remote_command_stream);

    let mut handle_remote_command = |avc_command: AvcCommand| {
        if avc_command.is_vendor_dependent() {
            let packet_body = avc_command.body();

            let preamble =
                VendorDependentPreamble::decode(packet_body).expect("unable to decode packet");

            let body = &packet_body[preamble.encoded_len()..];

            let pdu_id = PduId::try_from(preamble.pdu_id).expect("unknown PDU_ID");

            match pdu_id {
                PduId::GetElementAttributes => {
                    let _get_element_attributes_command = GetElementAttributesCommand::decode(body)
                        .expect("unable to decode get element attributes");

                    // We are making a massive response to test packet continuations work properly.
                    let element_attributes_response = GetElementAttributesResponse {
                        title: Some(String::from(&LOREM_IPSUM[0..100])),
                        artist_name: Some(String::from(&LOREM_IPSUM[500..])),
                        album_name: Some(String::from(&LOREM_IPSUM[250..500])),
                        genre: Some(String::from(&LOREM_IPSUM[100..250])),
                        ..GetElementAttributesResponse::default()
                    };
                    let mut packets = element_attributes_response
                        .encode_packets()
                        .expect("unable to encode packets for event");
                    let _ = avc_command
                        .send_response(AvcResponseType::ImplementedStable, &packets[0][..]);
                    drop(packets.remove(0));
                    additional_packets = packets;
                }
                PduId::RequestContinuingResponse => {
                    let request_cont_response = RequestContinuingResponseCommand::decode(body)
                        .expect("unable to decode continuting response");
                    assert_eq!(request_cont_response.pdu_id_response(), 0x20); // GetElementAttributes

                    let _ = avc_command.send_response(
                        AvcResponseType::ImplementedStable,
                        &additional_packets[0][..],
                    );
                    drop(additional_packets.remove(0));
                }
                PduId::RegisterNotification => {
                    let register_notification_command = RegisterNotificationCommand::decode(body)
                        .expect("unable to decode packet body");

                    assert!(
                        register_notification_command.event_id()
                            == &NotificationEventId::EventPlaybackPosChanged
                            || register_notification_command.event_id()
                                == &NotificationEventId::EventVolumeChanged
                    );

                    match register_notification_command.event_id() {
                        NotificationEventId::EventPlaybackPosChanged => {
                            position_changed_events += 1;

                            let intirm_response = PlaybackPosChangedNotificationResponse::new(
                                1000 * position_changed_events,
                            )
                            .encode_packet()
                            .expect("unable to encode pos response packet");
                            let _ = avc_command
                                .send_response(AvcResponseType::Interim, &intirm_response[..]);

                            // we are going to hang the response and not return an changed response after the 50th call.
                            // the last interim response should be
                            if position_changed_events < 50 {
                                let change_response = PlaybackPosChangedNotificationResponse::new(
                                    1000 * (position_changed_events + 1),
                                )
                                .encode_packet()
                                .expect("unable to encode pos response packet");
                                let _ = avc_command
                                    .send_response(AvcResponseType::Changed, &change_response[..]);
                            }
                        }
                        NotificationEventId::EventVolumeChanged => {
                            let intirm_response = VolumeChangedNotificationResponse::new(0x22)
                                .encode_packet()
                                .expect("unable to encode volume response packet");
                            let _ = avc_command
                                .send_response(AvcResponseType::Interim, &intirm_response[..]);

                            // we do not send an change response. We just hang it as if the volume never changes.
                        }
                        _ => assert!(false),
                    }
                }
                PduId::SetAbsoluteVolume => {
                    let set_absolute_volume_command =
                        SetAbsoluteVolumeCommand::decode(body).expect("unable to packet body");
                    assert_eq!(set_absolute_volume_command.volume(), REQUESTED_VOLUME);
                    let response = SetAbsoluteVolumeResponse::new(SET_VOLUME)
                        .expect("volume error")
                        .encode_packet()
                        .expect("unable to encode volume response packet");
                    let _ = avc_command.send_response(AvcResponseType::Accepted, &response[..]);
                }
                PduId::GetCapabilities => {
                    let get_capabilities_command =
                        GetCapabilitiesCommand::decode(body).expect("unable to packet body");
                    assert_eq!(
                        get_capabilities_command.capability_id(),
                        GetCapabilitiesCapabilityId::EventsId
                    );
                    // only notification types we claim to have are battery status, track pos, and volume
                    let response = GetCapabilitiesResponse::new_events(&[0x05, 0x06, 0x0d])
                        .encode_packet()
                        .expect("unable to encode response");
                    let _ = avc_command
                        .send_response(AvcResponseType::ImplementedStable, &response[..]);
                }
                PduId::GetPlayStatus => {
                    let _get_play_status_comand = GetPlayStatusCommand::decode(body)
                        .expect("GetPlayStatus: unable to packet body");
                    // Reply back with arbitrary status response. Song pos not supported.
                    let response = GetPlayStatusResponse {
                        song_length: 100,
                        song_position: 0xFFFFFFFF,
                        playback_status: PlaybackStatus::Stopped,
                    }
                    .encode_packet()
                    .expect("unable to encode response");
                    let _ = avc_command
                        .send_response(AvcResponseType::ImplementedStable, &response[..]);
                }
                PduId::GetCurrentPlayerApplicationSettingValue => {
                    let _get_player_application_settings_command =
                        GetCurrentPlayerApplicationSettingValueCommand::decode(body)
                            .expect("GetPlayerApplicationSettings: unable to packet body");
                    let response = GetCurrentPlayerApplicationSettingValueResponse::new(vec![(
                        player_application_settings::PlayerApplicationSettingAttributeId::Equalizer,
                        0x01,
                    )])
                    .encode_packet()
                    .expect("Unable to encode response");
                    let _ = avc_command
                        .send_response(AvcResponseType::ImplementedStable, &response[..]);
                }
                PduId::SetPlayerApplicationSettingValue => {
                    let _set_player_application_settings_command =
                        SetPlayerApplicationSettingValueCommand::decode(body)
                            .expect("SetPlayerApplicationSettings: unable to packet body");
                    let response = SetPlayerApplicationSettingValueResponse::new()
                        .encode_packet()
                        .expect("Unable to encode response");
                    let _ = avc_command.send_response(AvcResponseType::Accepted, &response[..]);
                }
                PduId::ListPlayerApplicationSettingAttributes => {
                    let _list_attributes_command =
                        ListPlayerApplicationSettingAttributesCommand::decode(body).expect(
                            "ListPlayerApplicationSettingAttributes: unable to packet body",
                        );
                    let response = ListPlayerApplicationSettingAttributesResponse::new(
                        1,
                        vec![PlayerApplicationSettingAttributeId::Equalizer],
                    )
                    .encode_packet()
                    .expect("Unable to encode response");
                    let _ = avc_command
                        .send_response(AvcResponseType::ImplementedStable, &response[..]);
                }
                PduId::GetPlayerApplicationSettingAttributeText => {
                    let _get_attribute_text_command =
                        GetPlayerApplicationSettingAttributeTextCommand::decode(body).expect(
                            "GetPlayerApplicationSettingAttributeText: unable to packet body",
                        );
                    let response = GetPlayerApplicationSettingAttributeTextResponse::new(vec![
                        AttributeInfo::new(
                            PlayerApplicationSettingAttributeId::Equalizer,
                            CharsetId::Utf8,
                            1,
                            vec![0x62],
                        ),
                    ])
                    .encode_packet()
                    .expect("Unable to encode response");
                    let _ = avc_command
                        .send_response(AvcResponseType::ImplementedStable, &response[..]);
                }
                PduId::ListPlayerApplicationSettingValues => {
                    let _list_value_command =
                        ListPlayerApplicationSettingValuesCommand::decode(body)
                            .expect("ListPlayerApplicationSettingValues: unable to packet body");
                    let response =
                        ListPlayerApplicationSettingValuesResponse::new(2, vec![0x01, 0x02])
                            .encode_packet()
                            .expect("Unable to encode response");
                    let _ = avc_command
                        .send_response(AvcResponseType::ImplementedStable, &response[..]);
                }
                PduId::GetPlayerApplicationSettingValueText => {
                    let _get_value_text_command =
                        GetPlayerApplicationSettingValueTextCommand::decode(body)
                            .expect("GetPlayerApplicationSettingValueText: unable to packet body");
                    let response =
                        GetPlayerApplicationSettingValueTextResponse::new(vec![ValueInfo::new(
                            1,
                            CharsetId::Utf8,
                            1,
                            vec![0x63],
                        )])
                        .encode_packet()
                        .expect("Unable to encode response");
                    let _ = avc_command
                        .send_response(AvcResponseType::ImplementedStable, &response[..]);
                }
                PduId::InformBatteryStatusOfCT => {
                    let received_command = InformBatteryStatusOfCtCommand::decode(body)
                        .expect("InformBatteryStatusOfCt: unable to decode packet body");
                    assert_eq!(received_command.battery_status(), current_battery_status);
                    let response = InformBatteryStatusOfCtResponse::new()
                        .encode_packet()
                        .expect("Unable to encode response");
                    let _ = avc_command.send_response(AvcResponseType::Accepted, &response[..]);
                }
                _ => {
                    // not entirely correct but just get it off our back for now.
                    let _ = avc_command.send_response(AvcResponseType::NotImplemented, &[]);
                }
            }
        } else {
            // Passthrough
            let body = avc_command.body();
            if body[0] & 0x80 == 0 {
                keydown_pressed = true;
            } else {
                keyup_pressed = true;
            }
            let key_code = body[0] & !0x80;
            let command = AvcPanelCommand::from_primitive(key_code);
            assert_eq!(command, Some(AvcPanelCommand::Key0));
            let _ = avc_command.send_response(AvcResponseType::Accepted, &[]);
        }
    };

    let mut last_receieved_pos = 0;

    loop {
        futures::select! {
            command = remote_command_stream.select_next_some() => {
                handle_remote_command(command?);
            }
            request = peer_controller_request_receiver.select_next_some()  => {
                peer_manager.handle_service_request(request);
            }
            res = handler_fut => {
                let _ = res?;
                assert!(false, "handler returned");
                drop(peer_manager_proxy);
                return Ok(())
            }
            res = test_handler_fut => {
                let _ = res?;
                assert!(false, "handler returned");
                drop(ext_proxy);
                return Ok(())
            }
            res = get_controller_fut => {
                let _ = res?;
            }
            res = get_test_controller_fut => {
                let _ = res?;
            }
            res = is_connected_fut => {
                assert_eq!(res?, true);
            }
            res = passthrough_fut => {
                expected_commands -= 1;
                assert_eq!(res?, Ok(()));
            }
            res = volume_fut => {
                expected_commands -= 1;
                assert_eq!(res?, Ok(SET_VOLUME));
            }
            res = events_fut => {
                expected_commands -= 1;
                let mut expected_set: HashSet<NotificationEvent> = [NotificationEvent::TrackPosChanged, NotificationEvent::BattStatusChanged, NotificationEvent::VolumeChanged].iter().cloned().collect();
                for item in res?.expect("supported events should be Ok") {
                    assert!(expected_set.remove(&item), "Missing {:?} in Events Supported.", item);
                }
            }
            res = get_media_attributes_fut => {
                expected_commands -= 1;
                let media_attributes = res?.expect("unable to parse media attributes");
                let expected = Some(String::from(&LOREM_IPSUM[100..250]));
                assert_eq!(media_attributes.genre, expected);
            }
            res = get_play_status_fut => {
                expected_commands -= 1;
                let play_status = res?.expect("unable to parse play status");
                assert_eq!(play_status.playback_status, Some(fidl_avrcp::PlaybackStatus::Stopped).into());
                assert_eq!(play_status.song_length, Some(100));
                assert_eq!(play_status.song_position, None);
            }
            res = get_player_application_settings_fut => {
                expected_commands -= 1;
                let settings = res?.expect("unable to parse get player application settings");
                assert!(settings.equalizer.is_some());
                assert!(settings.custom_settings.is_none());
                assert!(settings.scan_mode.is_none());
                assert!(settings.repeat_status_mode.is_none());
                assert!(settings.shuffle_mode.is_none());
                let eq = settings.equalizer.unwrap();
                assert_eq!(eq, fidl_avrcp::Equalizer::Off);
            }
            res = get_all_player_application_settings_fut => {
                expected_commands -= 1;
                let settings = res?.expect("unable to parse get player application settings");
                assert!(settings.equalizer.is_some());
                assert_eq!(settings.equalizer.unwrap(), fidl_avrcp::Equalizer::Off);
            }
            res = set_player_application_settings_fut => {
                expected_commands -= 1;
                let set_settings = res?.expect("unable to parse set player application settings");
                assert_eq!(set_settings.scan_mode, Some(fidl_avrcp::ScanMode::GroupScan));
                assert_eq!(set_settings.shuffle_mode, Some(fidl_avrcp::ShuffleMode::Off));
            }
            res = inform_battery_status_fut => {
                expected_commands -= 1;
                assert_eq!(res?, Ok(()));
            }
            event = event_stream.select_next_some() => {
                match event? {
                    ControllerEvent::OnNotification { timestamp: _, notification } => {
                        if let Some(value) = notification.pos {
                            last_receieved_pos = value;
                        }

                        if let Some(value) = notification.volume {
                            volume_value_received = true;
                            assert_eq!(value, 0x22);
                        }

                        controller_proxy.notify_notification_handled()?;
                    }
                }
            }
        }
        if expected_commands <= 0 && last_receieved_pos == 50 * 1000 && volume_value_received {
            assert!(keydown_pressed && keyup_pressed);
            return Ok(());
        }
    }
}

fn get_next_avctp_command(
    exec: &mut fasync::TestExecutor,
    command_stream: &mut AvctpCommandStream,
) -> AvctpCommand {
    exec.run_until_stalled(&mut command_stream.try_next())
        .expect("should be ready")
        .unwrap()
        .expect("has valid command")
}

/// Integration test of the peer manager and the FIDL front end with a mock BDEDR backend an
/// emulated remote peer. Validates we can get a browse controller to a device we discovered,
/// we can send browse commands on that controller, and that we can send responses
/// and have them be dispatched back as responses to the FIDL frontend in AVRCP.
/// Exercises a majority of the peer manager controller logic.
/// 1. Creates a front end FIDL endpoints for the browse controller interface, a peer manager, and mock
///    backend.
/// 2. It then creates a channel and injects a fake services discovered and incoming connection
///    event into the mock profile service.
/// 3. Obtains both a regular and test browse controller from the FIDL service.
/// 4. Test that appropriate commands are sent over the AVC/AVCTP channel and remote peer receives
///      a properly encoded message.
#[fuchsia::test]
fn peer_manager_with_browse_controller_client() {
    let mut exec = fasync::TestExecutor::new().expect("TestExecutor should be created");

    let fake_peer_id = PeerId(0);

    let (
        mut peer_manager,
        _profile_requests,
        peer_manager_proxy,
        ext_proxy,
        mut peer_controller_request_receiver,
    ) = spawn_peer_manager();

    let (local_avc, _remote_avc) = Channel::create();
    let (local_avctp, remote_avctp) = Channel::create();
    let remote_avctp_peer = AvctpPeer::new(remote_avctp);

    peer_manager.services_found(
        &fake_peer_id,
        vec![AvrcpService::Target {
            features: AvrcpTargetFeatures::CATEGORY1 | AvrcpTargetFeatures::SUPPORTSBROWSING,
            psm: Psm::AVCTP,
            protocol_version: AvrcpProtocolVersion(1, 6),
        }],
    );

    peer_manager.new_control_connection(&fake_peer_id, local_avc);
    peer_manager.new_browse_connection(&fake_peer_id, local_avctp);

    // Get browse controller client.
    let (browse_controller_proxy, browse_controller_server) =
        create_proxy().expect("should have initialized");
    let get_browse_controller_fut = peer_manager_proxy
        .get_browse_controller_for_target(&mut fake_peer_id.into(), browse_controller_server);
    pin_mut!(get_browse_controller_fut);

    exec.run_until_stalled(&mut get_browse_controller_fut).expect_pending("should be pending");
    peer_manager.handle_service_request(
        exec.run_until_stalled(&mut peer_controller_request_receiver.next())
            .expect("should be ready")
            .expect("should be some value"),
    );
    assert_matches!(
        exec.run_until_stalled(&mut get_browse_controller_fut),
        Poll::Ready(Ok(Ok(())))
    );

    // Get test browse controller client.
    let (browse_controller_ext_proxy, browse_controller_ext_server) =
        create_proxy().expect("should have initialized");
    let get_test_browse_controller_fut =
        ext_proxy.get_controller_for_target(&mut fake_peer_id.into(), browse_controller_ext_server);
    pin_mut!(get_test_browse_controller_fut);

    exec.run_until_stalled(&mut get_test_browse_controller_fut)
        .expect_pending("should not be ready");
    peer_manager.handle_service_request(
        exec.run_until_stalled(&mut peer_controller_request_receiver.next())
            .expect("should be ready")
            .expect("should be some value"),
    );
    assert_matches!(
        exec.run_until_stalled(&mut get_test_browse_controller_fut),
        Poll::Ready(Ok(Ok(())))
    );

    // Start testing actual commands.
    let mut avctp_cmd_stream = remote_avctp_peer.take_command_stream();

    // Test IsConnected.
    let mut is_browse_connected_fut = browse_controller_ext_proxy.is_connected();
    assert!(exec
        .run_until_stalled(&mut is_browse_connected_fut)
        .expect("should be ready")
        .unwrap());

    // Test GetNowPlayingItems, which should fail since SetBrowsedPlayer isn't set.
    let mut get_now_playing_fut = browse_controller_proxy.get_now_playing_items(
        START_INDEX,
        END_INDEX,
        &mut AttributeRequestOption::GetAll(true),
    );
    assert_matches!(exec.run_until_stalled(&mut get_now_playing_fut), Poll::Ready(Ok(Err(_))));

    // Test SetBrowsedPlayer.
    const PLAYER_ID: u16 = 1004;
    const UID_COUNTER: u16 = 1;
    let set_browsed_player_fut = browse_controller_proxy.set_browsed_player(PLAYER_ID);
    pin_mut!(set_browsed_player_fut);

    assert!(exec.run_until_stalled(&mut set_browsed_player_fut).is_pending());
    let command = get_next_avctp_command(&mut exec, &mut avctp_cmd_stream);
    // Ensure command params are correct.
    let params = decode_avctp_command(&command, PduId::SetBrowsedPlayer);
    let cmd = SetBrowsedPlayerCommand::decode(&params).expect("should have received valid command");
    assert_eq!(cmd.player_id(), PLAYER_ID);
    // Create mock response.
    let resp = SetBrowsedPlayerResponse::new_success(1, 1, vec!["folder1".to_string()])
        .expect("should be initialized");
    send_avctp_response(PduId::SetBrowsedPlayer, &resp, &command);

    assert_matches!(exec.run_until_stalled(&mut set_browsed_player_fut), Poll::Ready(Ok(Ok(()))));

    // Test GetMediaPlayerItems.
    const START_INDEX: u32 = 0;
    const END_INDEX: u32 = 5;
    let get_media_players_fut =
        browse_controller_proxy.get_media_player_items(START_INDEX, END_INDEX);
    pin_mut!(get_media_players_fut);

    assert!(exec.run_until_stalled(&mut get_media_players_fut).is_pending());
    let command = get_next_avctp_command(&mut exec, &mut avctp_cmd_stream);
    // Ensure command params are correct.
    let params = decode_avctp_command(&command, PduId::GetFolderItems);
    let cmd = GetFolderItemsCommand::decode(&params).expect("should have received valid command");
    assert_matches!(cmd.scope(),
    Scope::MediaPlayerList => {
        assert_eq!(cmd.start_item(), START_INDEX);
        assert_eq!(cmd.end_item(), END_INDEX);
        assert!(cmd.attribute_list().is_none());
    });
    // Create mock response.
    let resp = GetFolderItemsResponse::new_success(1, vec![]);
    send_avctp_response(PduId::GetFolderItems, &resp, &command);

    assert_matches!(
    exec.run_until_stalled(&mut get_media_players_fut),
    Poll::Ready(Ok(Ok(list))) => {
        assert_eq!(list.len(), 0);
    });

    // Test GetFileSystemItems. SetBrowsedPlayer was already called.
    let get_file_system_fut = browse_controller_proxy.get_file_system_items(
        START_INDEX,
        END_INDEX,
        &mut AttributeRequestOption::GetAll(false),
    );
    pin_mut!(get_file_system_fut);

    assert!(exec.run_until_stalled(&mut get_file_system_fut).is_pending());
    let command = get_next_avctp_command(&mut exec, &mut avctp_cmd_stream);
    // Ensure command params are correct.
    let params = decode_avctp_command(&command, PduId::GetFolderItems);
    let cmd = GetFolderItemsCommand::decode(&params).expect("should have received valid command");
    assert_matches!(cmd.scope(),
    Scope::MediaPlayerVirtualFilesystem => {
        assert_eq!(cmd.start_item(), START_INDEX);
                    assert_eq!(cmd.end_item(), END_INDEX);
                    assert_eq!(cmd.attribute_list().unwrap().len(), 0);
    });
    // Create mock response.
    let resp = GetFolderItemsResponse::new_success(1, vec![]);
    send_avctp_response(PduId::GetFolderItems, &resp, &command);

    assert_matches!(
        exec.run_until_stalled(&mut get_file_system_fut),
        futures::task::Poll::Ready(_)
    );

    // Test MoveIntoDirectory.
    let move_into_dir_fut = browse_controller_proxy.change_path(&mut Path::ChildFolderUid(2));
    pin_mut!(move_into_dir_fut);

    assert!(exec.run_until_stalled(&mut move_into_dir_fut).is_pending());
    let command = get_next_avctp_command(&mut exec, &mut avctp_cmd_stream);
    // Ensure command params are correct.
    let params = decode_avctp_command(&command, PduId::ChangePath);
    let cmd = ChangePathCommand::decode(&params).expect("should have received valid command");
    assert_eq!(cmd.uid_counter(), UID_COUNTER);
    assert_eq!(cmd.folder_uid(), Some(&std::num::NonZeroU64::new(2).unwrap()));
    // Create mock response.
    let resp = ChangePathResponse::new_success(10);
    send_avctp_response(PduId::ChangePath, &resp, &command);

    assert_matches!(
    exec.run_until_stalled(&mut move_into_dir_fut),
    Poll::Ready(Ok(Ok(num_of_items))) => {
        assert_eq!(num_of_items, 10);
    });
}
