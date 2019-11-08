// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:meta/meta.dart';

// Base class for actions handled by the tabs BLoC
class TabsAction<T> {
  final TabsActionType op;
  const TabsAction(this.op);
}

// Operations allowed for tab management
enum TabsActionType { newTab, removeTab, focusTab, addTab }

// Instructs to add a new tab to tab list.
class NewTabAction<T> extends TabsAction<T> {
  const NewTabAction() : super(TabsActionType.newTab);
}

// Instructs to remove a specific tab.
class RemoveTabAction<T> extends TabsAction<T> {
  final T tab;
  const RemoveTabAction({@required this.tab}) : super(TabsActionType.removeTab);
}

// Instructs to focus a specific tab.
class FocusTabAction<T> extends TabsAction<T> {
  final T tab;
  const FocusTabAction({@required this.tab}) : super(TabsActionType.focusTab);
}

// Instructs to add an existing tab to the tab list.
class AddTabAction<T> extends TabsAction<T> {
  final T tab;
  const AddTabAction({@required this.tab}) : super(TabsActionType.addTab);
}
