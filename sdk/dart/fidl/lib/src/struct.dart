// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ignore_for_file: public_member_api_docs

import 'codec.dart';
import 'hash_codes.dart';

abstract class Struct {
  const Struct();

  List<Object?> get $fields;

  @override
  int get hashCode => deepHash($fields);

  void $encode(Encoder encoder, int offset, int depth);

  @override
  bool operator ==(Object other) {
    if (identical(this, other)) {
      return true;
    }
    if (runtimeType != other.runtimeType) {
      return false;
    }
    if (other is Struct) {
      return deepEquals($fields, other.$fields);
    }
    return false;
  }

  @override
  String toString() {
    return '$runtimeType(${$fields})';
  }
}

typedef StructDecode<T> = T Function(Decoder decoder, int offset, int depth);
