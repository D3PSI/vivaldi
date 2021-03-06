// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/chromeos/login/user_creation_screen_handler.h"

#include "base/values.h"
#include "chrome/browser/ash/login/oobe_screen.h"
#include "chrome/browser/ash/login/screens/user_creation_screen.h"
#include "chrome/browser/ash/login/startup_utils.h"
#include "chrome/grit/chromium_strings.h"
#include "chrome/grit/generated_resources.h"
#include "components/login/localized_values_builder.h"
#include "ui/chromeos/devicetype_utils.h"

namespace chromeos {

constexpr StaticOobeScreenId UserCreationView::kScreenId;

UserCreationScreenHandler::UserCreationScreenHandler()
    : BaseScreenHandler(kScreenId) {
  set_user_acted_method_path_deprecated("login.UserCreationScreen.userActed");
}

UserCreationScreenHandler::~UserCreationScreenHandler() {
  if (screen_)
    screen_->OnViewDestroyed(this);
}

void UserCreationScreenHandler::DeclareLocalizedValues(
    ::login::LocalizedValuesBuilder* builder) {
  builder->AddF("userCreationTitle", IDS_OOBE_USER_CREATION_TITLE,
                ui::GetChromeOSDeviceName());
  builder->Add("userCreationSubtitle", IDS_OOBE_USER_CREATION_SUBTITLE);
  builder->AddF("userCreationAddPersonTitle",
                IDS_OOBE_USER_CREATION_ADD_PERSON_TITLE,
                ui::GetChromeOSDeviceName());
  builder->Add("userCreationAddPersonSubtitle",
               IDS_OOBE_USER_CREATION_ADD_PERSON_SUBTITLE);
  builder->Add("createForSelfLabel", IDS_OOBE_USER_CREATION_SELF_BUTTON_LABEL);
  builder->Add("createForSelfDescription",
               IDS_OOBE_USER_CREATION_SELF_BUTTON_DESCRIPTION);
  builder->Add("createForChildLabel",
               IDS_OOBE_USER_CREATION_CHILD_BUTTON_LABEL);
  builder->Add("createForChildDescription",
               IDS_OOBE_USER_CREATION_CHILD_BUTTON_DESCRIPTION);
  builder->AddF("childSignInTitle", IDS_OOBE_USER_CREATION_CHILD_SIGNIN_TITLE,
                ui::GetChromeOSDeviceName());
  builder->Add("childSignInSubtitle",
               IDS_OOBE_USER_CREATION_CHILD_SIGNIN_SUBTITLE);
  builder->Add("createAccountForChildLabel",
               IDS_OOBE_USER_CREATION_CHILD_ACCOUNT_CREATION_BUTTON_LABEL);
  builder->Add("signInForChildLabel",
               IDS_OOBE_USER_CREATION_CHILD_SIGN_IN_BUTTON_LABEL);
  builder->AddF("childSignInParentNotificationText",
                IDS_OOBE_USER_CREATION_CHILD_SIGN_IN_PARENT_NOTIFICATION_TEXT,
                ui::GetChromeOSDeviceName());
  builder->Add("childSignInLearnMore",
               IDS_OOBE_USER_CREATION_CHILD_SIGNIN_LEARN_MORE);
  builder->Add("childSignInLearnMoreDialogTitle",
               IDS_OOBE_USER_CREATION_CHILD_SIGN_IN_LEARN_MORE_DIALOG_TITLE);
  builder->Add("childSignInLearnMoreDialogText",
               IDS_OOBE_USER_CREATION_CHILD_SIGN_IN_LEARN_MORE_DIALOG_TEXT);
}

void UserCreationScreenHandler::InitializeDeprecated() {}

void UserCreationScreenHandler::Show() {
  ShowInWebUI();
}

void UserCreationScreenHandler::Bind(UserCreationScreen* screen) {
  screen_ = screen;
  BaseScreenHandler::SetBaseScreenDeprecated(screen_);
}

void UserCreationScreenHandler::Unbind() {
  screen_ = nullptr;
  BaseScreenHandler::SetBaseScreenDeprecated(nullptr);
}

void UserCreationScreenHandler::SetIsBackButtonVisible(bool value) {
  CallJS("login.UserCreationScreen.setIsBackButtonVisible", value);
}

}  // namespace chromeos
