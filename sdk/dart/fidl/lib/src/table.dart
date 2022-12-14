// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ignore_for_file: public_member_api_docs

import 'hash_codes.dart';
import 'unknown_data.dart';

abstract class Table {
  const Table();

  dynamic $field(int index);
  Map<int, dynamic> get $fields;
  Map<int, UnknownRawData>? get $unknownData;

  @override
  int get hashCode => deepHash($fields.values);

  @override
  bool operator ==(Object other) {
    if (identical(this, other)) {
      return true;
    }
    if (runtimeType != other.runtimeType) {
      return false;
    }
    if (other is Table) {
      return deepEquals($fields, other.$fields);
    }
    return false;
  }

  @override
  String toString() {
    return '$runtimeType(${$fields})';
  }
}

typedef TableFactory<T> = T Function(Map<int, dynamic> argv,
    [Map<int, UnknownRawData> unknownData]);
