/**
 * DO NOT EDIT
 *
 * This file was automatically generated by
 *   https://github.com/Polymer/tools/tree/master/packages/gen-typescript-declarations
 *
 * To modify these typings, edit the source file(s):
 *   animations/fade-in-animation.js
 */

import {Polymer} from '../../polymer/lib/legacy/polymer-fn.js';

import {NeonAnimationBehavior} from '../neon-animation-behavior.js';

import {LegacyElementMixin} from '../../polymer/lib/legacy/legacy-element-mixin.js';

/**
 * `<fade-in-animation>` animates the opacity of an element from 0 to 1.
 *
 * Configuration:
 * ```
 * {
 *   name: 'fade-in-animation',
 *   node: <node>
 *   timing: <animation-timing>
 * }
 * ```
 */
interface FadeInAnimationElement extends NeonAnimationBehavior, LegacyElementMixin, HTMLElement {
  configure(config: any): any;
}

export {FadeInAnimationElement};

declare global {

  interface HTMLElementTagNameMap {
    "fade-in-animation": FadeInAnimationElement;
  }
}
