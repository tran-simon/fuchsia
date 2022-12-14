// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This file tests FIDL's persistent encoding/decoding API.

use fidl::encoding::{persist, standalone_decode, standalone_encode, unpersist};
use fidl_fidl_rust_test_external::{Coordinate, FlexibleValueThing, ValueRecord};

// TODO(fxbug.dev/45252): Remove this.
fn transform_new_to_old_header(buf: &mut Vec<u8>) {
    //       disambiguator
    //            | magic
    //            |  | flags
    //            |  |  / \  ( reserved )
    //     new:  00 MA FL FL  00 00 00 00
    //     idx:  0  1  2  3   4  5  6  7
    //     old:  00 00 00 00  FL FL FL MA  00 00 00 00  00 00 00 00
    //          ( txid gap )   \ | /   |  (      ordinal gap      )
    //                         flags  magic
    let magic = buf[1];
    let flag1 = buf[2];
    let flag2 = buf[3];
    let new_header: [u8; 16] = [0, 0, 0, 0, flag1, flag2, 0, magic, 0, 0, 0, 0, 0, 0, 0, 0];
    buf.splice(0..8, new_header);
}

#[test]
fn persist_unpersist() {
    let mut body = Coordinate { x: 1, y: 2 };
    let buf = persist(&mut body).expect("encoding failed");
    let body_out = unpersist::<Coordinate>(&buf).expect("decoding failed");
    assert_eq!(body, body_out);
}

#[test]
fn persist_unpersist_presence() {
    let mut body = ValueRecord { name: Some("testing".to_string()), ..ValueRecord::EMPTY };
    let buf = persist(&mut body).expect("encoding failed");
    let body_out = unpersist::<ValueRecord>(&buf).expect("decoding failed");
    assert_eq!(body, body_out);
}

#[test]
fn persist_unpersist_with_old_header() {
    let mut body = Coordinate { x: 1, y: 2 };
    let mut buf = persist(&mut body).expect("encoding failed");
    transform_new_to_old_header(&mut buf);
    let body_out = unpersist::<Coordinate>(&buf).expect("decoding failed");
    assert_eq!(body, body_out);
}

#[test]
fn standalone_encode_decode_value() {
    let mut value = FlexibleValueThing::Name("foo".to_string());
    let (bytes, handles, metadata) = standalone_encode(&mut value).expect("encoding failed");
    assert_eq!(handles, &[]);
    let value_out = standalone_decode::<FlexibleValueThing>(&bytes, &mut [], &metadata)
        .expect("decoding failed");
    assert_eq!(value, value_out);
}

#[cfg(target_os = "fuchsia")]
mod zx {
    use super::*;
    use fidl::{encoding::convert_handle_dispositions_to_infos, AsHandleRef};
    use fidl_fidl_rust_test_external::StructWithHandles;
    use fuchsia_zircon as zx;

    #[test]
    fn standalone_encode_decode_resource() {
        let (c1, c2) = zx::Channel::create().expect("creating channel failed");
        let c1_koid = c1.get_koid();
        let c2_koid = c2.get_koid();

        let mut value = StructWithHandles { v: vec![c1, c2] };

        let (bytes, handle_dispositions, metadata) =
            standalone_encode(&mut value).expect("encoding failed");
        assert_eq!(handle_dispositions.len(), 2);

        let mut handle_infos = convert_handle_dispositions_to_infos(handle_dispositions)
            .expect("converting to handle infos failed");
        let value_out =
            standalone_decode::<StructWithHandles>(&bytes, &mut handle_infos, &metadata)
                .expect("decoding failed");

        assert_eq!(value_out.v[0].get_koid(), c1_koid);
        assert_eq!(value_out.v[1].get_koid(), c2_koid);
    }
}
