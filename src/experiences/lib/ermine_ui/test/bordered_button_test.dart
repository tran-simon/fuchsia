// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:ermine_ui/ermine_ui.dart';

void main() {
  testWidgets(
      'Small button should have the given label, and onTap callback, '
      'and have the text style corresponding to the chosen type.',
      (WidgetTester tester) async {
    var isTapped = false;
    final button = BorderedButton.small('small', () => isTapped = true);

    await tester.pumpWidget(_buildButton(button));

    // The label text should be correct and capitalized.
    expect(find.text('SMALL'), findsOneWidget);

    // [onTap] should work as intended when tapped.
    expect(isTapped, isFalse);
    await tester.tap(find.byType(BorderedButton));
    expect(isTapped, isTrue);

    final textStyle =
        // ignore: avoid_as
        (tester.widget(find.byType(BorderedButton)) as BorderedButton)
            .textStyle;
    expect(textStyle, ErmineTextStyles.bodyText2);
  });

  testWidgets(
      'Medium button should have the given label, and onTap callback, '
      'and have the text style corresponding to the chosen type.',
      (WidgetTester tester) async {
    var isTapped = false;
    final button = BorderedButton.medium('medium', () => isTapped = true);

    await tester.pumpWidget(_buildButton(button));

    // The label text should be correct and capitalized.
    expect(find.text('MEDIUM'), findsOneWidget);

    // [onTap] should work as intended when tapped.
    expect(isTapped, isFalse);
    await tester.tap(find.byType(BorderedButton));
    expect(isTapped, isTrue);

    final textStyle =
        // ignore: avoid_as
        (tester.widget(find.byType(BorderedButton)) as BorderedButton)
            .textStyle;
    expect(textStyle, ErmineTextStyles.subtitle1);
  });

  testWidgets(
      'Large button should have the given label, and onTap callback, '
      'and have the text style corresponding to the chosen type.',
      (WidgetTester tester) async {
    var isTapped = false;
    final button = BorderedButton.large('large', () => isTapped = true);

    await tester.pumpWidget(_buildButton(button));

    // The label text should be correct and capitalized.
    expect(find.text('LARGE'), findsOneWidget);

    // [onTap] should work as intended when tapped.
    expect(isTapped, isFalse);
    await tester.tap(find.byType(BorderedButton));
    expect(isTapped, isTrue);

    final textStyle =
        // ignore: avoid_as
        (tester.widget(find.byType(BorderedButton)) as BorderedButton)
            .textStyle;
    expect(textStyle, ErmineTextStyles.headline3);
  });

  testWidgets('The small button should be bigger than or equal to 24x24 ',
      (WidgetTester tester) async {
    const min = 24;
    final button = BorderedButton.small('', () {});

    await tester.pumpWidget(_buildButton(button));
    final size = tester.getSize(find.byType(BorderedButton));
    expect(size.width, greaterThanOrEqualTo(min));
    expect(size.height, greaterThanOrEqualTo(min));
  });
  testWidgets('The medium button should be bigger than or equal to 30x30',
      (WidgetTester tester) async {
    const min = 30;
    final button = BorderedButton.medium('', () {});

    // Medium text-only button should be bigger than or equal to 30 x 30.
    await tester.pumpWidget(_buildButton(button));
    final sizeTextM = tester.getSize(find.byType(BorderedButton));
    expect(sizeTextM.width, greaterThanOrEqualTo(min));
    expect(sizeTextM.height, greaterThanOrEqualTo(min));
  });

  testWidgets('The large button should be bigger than or equal to 36x36',
      (WidgetTester tester) async {
    const min = 36;
    final button = BorderedButton.large('', () {});

    await tester.pumpWidget(_buildButton(button));
    final sizeTextM = tester.getSize(find.byType(BorderedButton));
    expect(sizeTextM.width, greaterThanOrEqualTo(min));
    expect(sizeTextM.height, greaterThanOrEqualTo(min));
  });
}

Widget _buildButton(ErmineButton button) => MaterialApp(
      home: Scaffold(
        body: button,
      ),
    );
