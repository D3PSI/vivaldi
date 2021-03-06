// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <utility>

#include "chrome/browser/ui/views/webauthn/authenticator_select_account_sheet_view.h"
#include "chrome/browser/ui/views/webauthn/hover_list_view.h"

AuthenticatorSelectAccountSheetView::AuthenticatorSelectAccountSheetView(
    std::unique_ptr<AuthenticatorSelectAccountSheetModel> sheet_model)
    : AuthenticatorRequestSheetView(std::move(sheet_model)) {}

AuthenticatorSelectAccountSheetView::~AuthenticatorSelectAccountSheetView() =
    default;

std::pair<std::unique_ptr<views::View>,
          AuthenticatorRequestSheetView::AutoFocus>
AuthenticatorSelectAccountSheetView::BuildStepSpecificContent() {
  return std::make_pair(
      std::make_unique<HoverListView>(std::make_unique<AccountHoverListModel>(
          &model()->dialog_model()->creds(), this)),
      AutoFocus::kYes);
}

void AuthenticatorSelectAccountSheetView::OnItemSelected(int index) {
  auto* sheet_model =
      static_cast<AuthenticatorSelectAccountSheetModel*>(model());
  sheet_model->SetCurrentSelection(index);
  sheet_model->OnAccept();
}
