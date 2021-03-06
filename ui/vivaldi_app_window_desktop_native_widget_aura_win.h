// Copyright (c) 2017 Vivaldi Technologies AS. All rights reserved.
//
// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_VIVALDI_APP_WINDOW_DESKTOP_NATIVE_WIDGET_AURA_WIN_H_
#define UI_VIVALDI_APP_WINDOW_DESKTOP_NATIVE_WIDGET_AURA_WIN_H_

#include "ui/views/widget/desktop_aura/desktop_native_widget_aura.h"

class VivaldiNativeAppWindowViewsWin;

namespace views {
class DesktopWindowTreeHost;
}

// A DesktopNativeWidgetAura subclass that handles creating the right type of
// tree hosts for app windows on Windows. It is based on
// AppWindowDesktopNativeWidgetAuraWin.
class VivaldiAppWindowDesktopNativeWidgetAuraWin
    : public views::DesktopNativeWidgetAura {
 public:
  explicit VivaldiAppWindowDesktopNativeWidgetAuraWin(
      VivaldiNativeAppWindowViewsWin* app_window);
  VivaldiAppWindowDesktopNativeWidgetAuraWin(
      const VivaldiAppWindowDesktopNativeWidgetAuraWin&) = delete;
  VivaldiAppWindowDesktopNativeWidgetAuraWin& operator=(
      const VivaldiAppWindowDesktopNativeWidgetAuraWin&) = delete;

 protected:
  ~VivaldiAppWindowDesktopNativeWidgetAuraWin() override;

  // Overridden from views::DesktopNativeWidgetAura:
  void InitNativeWidget(views::Widget::InitParams params) override;
  void Maximize() override;
  void Minimize() override;
  void OnHostWorkspaceChanged(aura::WindowTreeHost* host) override;

 private:
  // Ownership managed by the views system.
  VivaldiNativeAppWindowViewsWin* app_window_;

  // Owned by superclass DesktopNativeWidgetAura.
  views::DesktopWindowTreeHost* tree_host_;
};

#endif  // UI_VIVALDI_APP_WINDOW_DESKTOP_NATIVE_WIDGET_AURA_WIN_H_
