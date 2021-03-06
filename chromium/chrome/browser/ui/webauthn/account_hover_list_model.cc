// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webauthn/account_hover_list_model.h"

#include "base/strings/utf_string_conversions.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "chrome/grit/generated_resources.h"
#include "device/fido/authenticator_get_assertion_response.h"
#include "device/fido/discoverable_credential_metadata.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/paint_vector_icon.h"

AccountHoverListModel::AccountHoverListModel(
    const std::vector<device::DiscoverableCredentialMetadata>* creds,
    Delegate* delegate)
    : creds_(creds), delegate_(delegate) {}

AccountHoverListModel::~AccountHoverListModel() = default;

bool AccountHoverListModel::ShouldShowPlaceholderForEmptyList() const {
  return false;
}

std::u16string AccountHoverListModel::GetPlaceholderText() const {
  return std::u16string();
}

const gfx::VectorIcon* AccountHoverListModel::GetPlaceholderIcon() const {
  return &kUserAccountAvatarIcon;
}

std::vector<int> AccountHoverListModel::GetThrobberTags() const {
  return {};
}

std::vector<int> AccountHoverListModel::GetButtonTags() const {
  std::vector<int> tag_list(creds_->size());
  for (size_t i = 0; i < creds_->size(); ++i)
    tag_list[i] = i;
  return tag_list;
}

std::u16string AccountHoverListModel::GetItemText(int item_tag) const {
  const device::PublicKeyCredentialUserEntity& user = creds_->at(item_tag).user;
  if (user.display_name && !user.display_name->empty())
    return base::UTF8ToUTF16(user.display_name.value());
  return l10n_util::GetStringUTF16(IDS_WEBAUTHN_UNKNOWN_ACCOUNT);
}

std::u16string AccountHoverListModel::GetDescriptionText(int item_tag) const {
  const device::PublicKeyCredentialUserEntity& user = creds_->at(item_tag).user;
  return base::UTF8ToUTF16(user.name.value_or(""));
}

const gfx::VectorIcon* AccountHoverListModel::GetItemIcon(int item_tag) const {
  return nullptr;
}

void AccountHoverListModel::OnListItemSelected(int item_tag) {
  delegate_->OnItemSelected(item_tag);
}

size_t AccountHoverListModel::GetPreferredItemCount() const {
  return creds_->size();
}

bool AccountHoverListModel::StyleForTwoLines() const {
  return true;
}
