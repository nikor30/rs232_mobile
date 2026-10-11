// Lets xterm.js run under this page's Content-Security-Policy (style-src 'self').
// xterm styles itself with <style> elements and with style attributes; the
// policy refuses both, and the terminal then loses its layout. The style sheet
// objects and the style properties that scripts set directly are not affected by
// the policy, so both are redirected there. Must be loaded before xterm.js.
(() => {
  'use strict';

  // Stands in for a <style> element: its text becomes a constructed style sheet
  // for as long as the element is in the document.
  class CspStyle extends HTMLElement {
    constructor() {
      super();
      this.cssSheet = new CSSStyleSheet();
      new MutationObserver(() => this.sync()).observe(this, { childList: true, characterData: true, subtree: true });
    }
    sync() {
      try { this.cssSheet.replaceSync(this.textContent); } catch (e) { /* a rule the browser does not know */ }
    }
    connectedCallback() {
      this.hidden = true;                           // the text is CSS, not something to show
      this.sync();
      if (!document.adoptedStyleSheets.includes(this.cssSheet)) document.adoptedStyleSheets = [...document.adoptedStyleSheets, this.cssSheet];
    }
    disconnectedCallback() {
      document.adoptedStyleSheets = document.adoptedStyleSheets.filter((s) => s !== this.cssSheet);
    }
  }
  customElements.define('csp-style', CspStyle);

  const createElement = Document.prototype.createElement;
  Document.prototype.createElement = function (name, options) {
    return createElement.call(this, String(name).toLowerCase() === 'style' ? 'csp-style' : name, options);
  };

  const setAttribute = Element.prototype.setAttribute;
  Element.prototype.setAttribute = function (name, value) {
    if (String(name).toLowerCase() === 'style' && this.style) this.style.cssText = value;
    else setAttribute.call(this, name, value);
  };
})();
