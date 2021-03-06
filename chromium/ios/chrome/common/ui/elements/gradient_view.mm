// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/common/ui/elements/gradient_view.h"

#import "base/mac/foundation_util.h"
#import "ios/chrome/common/ui/colors/semantic_color_names.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

@implementation GradientView

#pragma mark - Public

+ (Class)layerClass {
  return [CAGradientLayer class];
}

- (instancetype)init {
  self = [super initWithFrame:CGRectZero];
  if (self) {
    self.userInteractionEnabled = NO;
    [self updateColors];
  }
  return self;
}

- (CAGradientLayer*)gradientLayer {
  return base::mac::ObjCCastStrict<CAGradientLayer>(self.layer);
}

- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if ([self.traitCollection
          hasDifferentColorAppearanceComparedToTraitCollection:
              previousTraitCollection]) {
    [self updateColors];
  }
}

#pragma mark - Private

- (void)updateColors {
  [CATransaction begin];
  // If this isn't set, the changes here are automatically animated. The other
  // color changes for dark mode don't animate, however, so there ends up being
  // visual desyncing.
  [CATransaction setDisableActions:YES];

  self.gradientLayer.colors = @[
    (id)[[UIColor colorNamed:kPrimaryBackgroundColor] colorWithAlphaComponent:0]
        .CGColor,
    (id)[UIColor colorNamed:kPrimaryBackgroundColor].CGColor,
  ];
  [CATransaction commit];
}

@end
