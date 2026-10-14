import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { applyAppearanceConfig, onCandidateSurfaceThemeChanged, setupAppearance } from './appearance';
import partial from '../partials/appearance.html?raw';

const transforms = vi.hoisted(() => new Map<string, (value: string) => string>());
vi.mock('./shared', () => ({
  applyDropdownValue: vi.fn(), applyToggleState: vi.fn(), populateDropdownMenu: vi.fn(),
  registerDropdownPreparer: vi.fn(), setupToggleButton: vi.fn(),
  isWebView2UiBackend: (backend: string | undefined) =>
    backend === 'webview2' || backend === 'webview' || backend === 'web',
  setupDropdownMenu: (_button: string, menu: string, _action: string, _stop: boolean,
                      _path: string, transform: (value: string) => string) => transforms.set(menu, transform)
}));
vi.mock('./theme', () => ({ applyThemeConfig: vi.fn(), setCandidateSurfaceThemeListener: vi.fn() }));
vi.mock('../utils/common-utils', () => ({ loadHTML: async () => '' }));

class Element extends EventTarget {
  children: Element[] = [];
  textContent = '';
  value = '';
  dataset: Record<string, string> = {};
  id = '';
  className = '';
  disabled = false;
  hidden = false;
  parent?: Element;
  style = { setProperty: vi.fn(), removeProperty: vi.fn() };
  classList = { toggle: vi.fn(), add: vi.fn(), remove: vi.fn() };
  appendChild(child: Element) { child.parent = this; this.children.push(child); }
  replaceChildren() { this.children = []; }
  setAttribute() {}
  querySelector(selector: string): Element | undefined { return this.querySelectorAll(selector)[0]; }
  querySelectorAll(selector: string): Element[] { return this.children.filter((child) => child.className.split(' ').includes(selector.slice(1))); }
  remove() { if (this.parent) this.parent.children = this.parent.children.filter((child) => child !== this); }
  select() {}
  focus() {}
}

let list: Element;
let preview: Element;
let caretPreview: Element;
let root: Element;
let backgroundOpacityInput: Element;
let backgroundOpacityDec: Element;
let backgroundOpacityInc: Element;
let foregroundOpacityInput: Element;
let foregroundOpacityDec: Element;
let foregroundOpacityInc: Element;
let opacitySplitRows: Element;
let opacitySingleRow: Element;
let opacityInput: Element;
let opacityDec: Element;
let opacityInc: Element;
let postMessage: ReturnType<typeof vi.fn>;
beforeEach(() => {
  list = new Element();
  preview = new Element();
  root = new Element();
  caretPreview = new Element();
  caretPreview.id = 'caretStatePreviewHost';
  backgroundOpacityInput = new Element();
  backgroundOpacityInput.id = 'candBackgroundOpacityInput';
  backgroundOpacityDec = new Element();
  backgroundOpacityDec.id = 'candBackgroundOpacityDecBtn';
  backgroundOpacityInc = new Element();
  backgroundOpacityInc.id = 'candBackgroundOpacityIncBtn';
  foregroundOpacityInput = new Element();
  foregroundOpacityInput.id = 'candForegroundOpacityInput';
  foregroundOpacityDec = new Element();
  foregroundOpacityDec.id = 'candForegroundOpacityDecBtn';
  foregroundOpacityInc = new Element();
  foregroundOpacityInc.id = 'candForegroundOpacityIncBtn';
  opacitySplitRows = new Element();
  opacitySplitRows.id = 'candOpacitySplitRows';
  opacitySingleRow = new Element();
  opacitySingleRow.id = 'candOpacitySingleRow';
  opacitySingleRow.hidden = true;
  opacityInput = new Element();
  opacityInput.id = 'candOpacityInput';
  opacityDec = new Element();
  opacityDec.id = 'candOpacityDecBtn';
  opacityInc = new Element();
  opacityInc.id = 'candOpacityIncBtn';
  postMessage = vi.fn();
  vi.stubGlobal('window', { chrome: { webview: { postMessage } } });
  vi.stubGlobal('document', {
    getElementById: (id: string) => {
      const find = (element: Element): Element | undefined => element.id === id ? element : element.children.map(find).find(Boolean);
      return id === 'candFallbackFontList' ? list : id.startsWith('candidate-wnd-') ? preview
        : id === 'caretStatePreviewHost' ? caretPreview
        : id === 'candBackgroundOpacityInput' ? backgroundOpacityInput
        : id === 'candBackgroundOpacityDecBtn' ? backgroundOpacityDec
        : id === 'candBackgroundOpacityIncBtn' ? backgroundOpacityInc
        : id === 'candForegroundOpacityInput' ? foregroundOpacityInput
        : id === 'candForegroundOpacityDecBtn' ? foregroundOpacityDec
        : id === 'candForegroundOpacityIncBtn' ? foregroundOpacityInc
        : id === 'candOpacitySplitRows' ? opacitySplitRows
        : id === 'candOpacitySingleRow' ? opacitySingleRow
        : id === 'candOpacityInput' ? opacityInput
        : id === 'candOpacityDecBtn' ? opacityDec
        : id === 'candOpacityIncBtn' ? opacityInc
        : find(list);
    },
    documentElement: root,
    addEventListener: vi.fn(),
    querySelector: () => null,
    querySelectorAll: (selector: string) => selector.startsWith('.cand-preview .candidate') ? [preview] : [],
    createElement: () => new Element()
  });
});
afterEach(() => vi.unstubAllGlobals());

function selects() { return list.children.filter((element) => element.className === 'dropdown fallback-font-dropdown').map((element) => element.children[0]); }
function labels() { return selects().map((button) => button.children[0].value).filter(Boolean); }
function choose(index: number, value: string) { return transforms.get(`fallbackFontMenu${index}`)!(value); }
function family() { return (preview.style as unknown as { fontFamily: string }).fontFamily; }

it('shows ordered selectors without separators and saves replacement/removal', () => {
  applyAppearanceConfig(undefined, undefined, undefined, {
    english_font: 'Segoe UI', fallback_fonts: ['Font A', 'Font B'],
    fallback_font_css_families: ['Family A', 'Family B']
  });
  expect(family()).toBe('"Segoe UI", "Family A", "Family B", sans-serif');
  expect(list.children.map((element) => element.className)).toEqual([
    'dropdown fallback-font-dropdown', 'dropdown fallback-font-dropdown', 'dropdown-toggle fallback-font-add'
  ]);
  const saved = choose(1, 'Font C');
  expect(labels()).toEqual(['Font A', 'Font C']);
  expect(family()).toBe('"Segoe UI", "Family A", "Font C", sans-serif');
  expect(saved).toBe('["Font A","Font C"]');
  choose(0, '');
  expect(labels()).toEqual(['Font C']);
});

it('publishes the candidate font on the root for the skin list previews', () => {
  applyAppearanceConfig(undefined, undefined, undefined, {
    english_font: 'Segoe UI', fallback_fonts: ['Font A'], fallback_font_css_families: ['Family A']
  });
  expect(root.style.setProperty).toHaveBeenLastCalledWith('--cand-font-family', '"Segoe UI", "Family A", sans-serif');
});

it('keeps an explicit empty list and quotes punctuation inside font names', () => {
  applyAppearanceConfig(undefined, undefined, undefined, {
    english_font: 'Segoe UI', fallback_fonts: ['Font,"x\\']
  });
  expect(family()).toBe('"Segoe UI", "Font,\\"x\\\\", sans-serif');
  applyAppearanceConfig(undefined, undefined, undefined, { fallback_fonts: [] });
  expect(labels()).toEqual([]);
  expect(family()).toBe('"Segoe UI", sans-serif');
});

it('adds one empty selector with the plus button and only saves after choosing a font', async () => {
  await setupAppearance();
  applyAppearanceConfig(undefined, undefined, undefined, { fallback_fonts: ['SimSun'] });
  list.children.at(-1)!.dispatchEvent(new Event('click'));
  expect(selects().map((button) => button.children[0].value)).toEqual(['SimSun', '']);
  expect(selects()[0].className).toBe('dropdown-toggle font-combobox fallback-font-select');
  expect(list.children.at(-1)!.className).toBe('dropdown-toggle fallback-font-add');
  expect(postMessage).not.toHaveBeenCalled();
  const saved = choose(1, 'DengXian');
  expect(labels()).toEqual(['SimSun', 'DengXian']);
  expect(saved).toBe('["SimSun","DengXian"]');
});

it('steps the candidate window background and foreground opacity within 1–100 and persists each change', async () => {
  await setupAppearance();
  // 快照回填：两个值各落进自己的输入框，± 的禁用态按区间刷新。
  applyAppearanceConfig(undefined, undefined, undefined, {
    candidate_window_background_opacity: 42,
    candidate_window_foreground_opacity: 80
  });
  expect(backgroundOpacityInput.value).toBe('42');
  expect(foregroundOpacityInput.value).toBe('80');
  expect(backgroundOpacityDec.disabled).toBe(false);
  expect(backgroundOpacityInc.disabled).toBe(false);

  backgroundOpacityInc.dispatchEvent(new Event('click'));
  expect(backgroundOpacityInput.value).toBe('43');
  expect(postMessage).toHaveBeenCalledTimes(1);
  expect(postMessage.mock.calls[0][0]).toContain('appearance.candidate_window_background_opacity');
  expect(postMessage.mock.calls[0][0]).toContain('43');

  // 前景走自己的配置键。
  postMessage.mockClear();
  foregroundOpacityInc.dispatchEvent(new Event('click'));
  expect(postMessage.mock.calls[0][0]).toContain('appearance.candidate_window_foreground_opacity');
  expect(postMessage.mock.calls[0][0]).toContain('81');

  // 背景到上限再按 +：值不变，不上报。
  postMessage.mockClear();
  applyAppearanceConfig(undefined, undefined, undefined, { candidate_window_background_opacity: 100 });
  expect(backgroundOpacityInc.disabled).toBe(true);
  backgroundOpacityInc.dispatchEvent(new Event('click'));
  expect(postMessage).not.toHaveBeenCalled();
  expect(backgroundOpacityInput.value).toBe('100');

  backgroundOpacityDec.dispatchEvent(new Event('click'));
  expect(postMessage.mock.calls[0][0]).toContain('99');
});

it('keeps the candidate opacity rows addressable and the WebView2 one hidden by default', () => {
  const split = partial.match(/id="candOpacitySplitRows"([\s\S]*?)id="candOpacitySingleRow"/)?.[1] ?? '';
  // 拆开的那块装着背景与前景两个选择器，收起来的那块只有一个，且初始收起
  // （出厂默认后端是 D2D）。
  expect(split).toContain('id="candBackgroundOpacityDecBtn"');
  expect(split).toContain('id="candForegroundOpacityDecBtn"');
  expect(partial).toMatch(/id="candOpacitySingleRow"\s+hidden/);
  for (const id of ['candOpacityDecBtn', 'candOpacityInput', 'candOpacityIncBtn']) {
    expect(partial).toContain(`id="${id}"`);
  }
});

it('shows one opacity option under WebView2 and the foreground/background pair under D2D', async () => {
  await setupAppearance();
  // D2D：前景/背景各一个，单选项那块收起来。
  applyAppearanceConfig(undefined, undefined, undefined, {
    ui_backend: 'd2d', candidate_window_background_opacity: 42
  });
  expect(opacitySplitRows.hidden).toBe(false);
  expect(opacitySingleRow.hidden).toBe(true);
  expect(opacityInput.value).toBe('42');

  // WebView2：宿主只有整窗一个 alpha，前景那个对它无效，整块收成一个选项。
  applyAppearanceConfig(undefined, undefined, undefined, {
    ui_backend: 'webview2', candidate_window_background_opacity: 42
  });
  expect(opacitySplitRows.hidden).toBe(true);
  expect(opacitySingleRow.hidden).toBe(false);
  expect(opacityInput.value).toBe('42');
  expect(opacityDec.disabled).toBe(false);
  expect(opacityInc.disabled).toBe(false);

  // 那一个选项绑的就是「背景不透明度」这个键：WebView2 的整窗 alpha 取的就是它。
  postMessage.mockClear();
  opacityInc.dispatchEvent(new Event('click'));
  expect(postMessage.mock.calls[0][0]).toContain('appearance.candidate_window_background_opacity');
  expect(postMessage.mock.calls[0][0]).toContain('43');

  // 快照没带 ui_backend 时沿用上次的值，不回退。
  applyAppearanceConfig(undefined, undefined, undefined, { candidate_window_background_opacity: 50 });
  expect(opacitySplitRows.hidden).toBe(true);
  expect(opacitySingleRow.hidden).toBe(false);

  // 别名 web/webview 也是 WebView2，其余（含旧配置）落 D2D。
  applyAppearanceConfig(undefined, undefined, undefined, { ui_backend: 'web' });
  expect(opacitySplitRows.hidden).toBe(true);
  applyAppearanceConfig(undefined, undefined, undefined, { ui_backend: 'd2d' });
  expect(opacitySplitRows.hidden).toBe(false);
  expect(opacitySingleRow.hidden).toBe(true);
});

it('keeps the caret preview on the resolved candidate theme and text color', () => {
  applyAppearanceConfig(undefined, undefined, undefined, { cand_text_color: '#123456' });
  expect(caretPreview.style.setProperty).toHaveBeenCalledWith('--caret-text-override', '#123456');

  onCandidateSurfaceThemeChanged('light');
  expect(caretPreview.classList.toggle).toHaveBeenCalledWith('theme-light', true);
  expect(caretPreview.classList.toggle).toHaveBeenCalledWith('theme-dark', false);

  applyAppearanceConfig(undefined, undefined, undefined, { cand_text_color: 'auto' });
  expect(caretPreview.style.removeProperty).toHaveBeenCalledWith('--caret-text-override');
});

it('filters each supplementary font menu independently without persisting search text', () => {
  applyAppearanceConfig(undefined, undefined, undefined, {
    fallback_fonts: ['SimSun', 'DengXian'], system_fonts: ['SimSun', 'SimHei', 'DengXian']
  });
  const input = selects()[0].children[0];
  input.value = 'sIM';
  input.dispatchEvent(new Event('input'));
  const dropdowns = list.children.filter((item) => item.className === 'dropdown fallback-font-dropdown');
  const menu = dropdowns[0].children[1];
  expect(menu.children.filter((item) => !item.hidden).map((item) => item.textContent)).toEqual(['SimSun', 'SimHei']);
  expect(dropdowns[1].children[1].children.every((item) => !item.hidden)).toBe(true);
  expect(postMessage).not.toHaveBeenCalled();
  input.value = 'no such font';
  input.dispatchEvent(new Event('input'));
  expect(menu.children.at(-1)!.textContent).toBe('没有匹配的字体');
  input.value = '';
  input.dispatchEvent(new Event('input'));
  expect(menu.children.map((item) => item.textContent)).toEqual(['移除此字体', 'SimSun', 'SimHei', 'DengXian']);
});
