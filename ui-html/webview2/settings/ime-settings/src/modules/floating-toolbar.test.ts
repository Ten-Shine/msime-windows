/// <reference types="node" />
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { expect, it, vi } from 'vitest';
import {
  applyCaretStateIndicatorPosition,
  applyFloatingToolbarAppearanceConfig,
  applyFloatingToolbarAutoDimConfig,
  applyFloatingToolbarAutoHideConfig,
  applyFloatingToolbarShadowConfig,
  clampAutoDimDelay,
  clampAutoHideDelay,
  setupFloatingToolbar
} from './floating-toolbar';
import { setupDropdownMenu, applyDropdownValue, applyToggleState, setupToggleButton } from './shared';
import { updateConfig } from './config-sync';
import partial from '../partials/floating-toolbar.html?raw';

vi.mock('./shared', () => ({
  setupDropdownMenu: vi.fn(),
  applyDropdownValue: vi.fn(),
  applyToggleState: vi.fn(),
  setupToggleButton: vi.fn(),
  isWebView2UiBackend: (backend: string | undefined) =>
    backend === 'webview2' || backend === 'webview' || backend === 'web'
}));
vi.mock('./config-sync', () => ({ updateConfig: vi.fn() }));
vi.mock('./appearance', () => ({ syncCaretStateIndicatorPreview: vi.fn() }));
vi.mock('./skin', () => ({ syncAppearancePreviews: vi.fn() }));

const styles = readFileSync(fileURLToPath(new URL('../styles/modules/floating-toolbar.css', import.meta.url)), 'utf8');

it('keeps toolbar and caret controls in separate cards with separate previews', () => {
  const toolbarCard = partial.match(/<div class="section floating-toolbar-card">([\s\S]*?)<div class="section caret-state-indicator-card">/)?.[1] ?? '';
  const caretCard = partial.match(/<div class="section caret-state-indicator-card">([\s\S]*?)<div class="section floating-toolbar-appearance">/)?.[1] ?? '';

  expect(toolbarCard).toContain('id="ftbToggleBtn"');
  expect(toolbarCard).toContain('id="ftbPreviewHost"');
  expect(toolbarCard).not.toContain('caretStateIndicator');
  expect(caretCard).toContain('class="ftb-toggle-btn" id="caretStateIndicatorToggleBtn" role="switch" aria-label="切换输入法状态时提示" aria-checked="false"');
  expect(caretCard).toContain('可与悬浮工具栏同时开启');
  expect(caretCard).not.toContain('关闭悬浮工具栏后');
  expect(caretCard).toContain('id="caretStateIndicatorPositionBtn"');
  expect(caretCard).toContain('class="ftb-toggle-btn" id="caretStateIndicatorOnFocusToggleBtn" role="switch" aria-label="切换输入框时提示当前中英文状态" aria-checked="false"');
  expect(caretCard).toContain('id="caretStatePreviewHost"');
  expect(caretCard).not.toContain('id="ftbPreviewHost"');
  expect(caretCard).toContain('role="img" aria-label="光标状态提示预览：每个文字光标左上方分别显示中、中文标点和中文模式、全角、简体"');
  expect(caretCard).toContain('class="cand-preview" aria-hidden="true"');
});

it('titles the caret card as a group whose settings sit under it', () => {
  const caretCard = partial.match(/<div class="section caret-state-indicator-card">([\s\S]*?)<div class="section floating-toolbar-appearance">/)?.[1] ?? '';
  const heading = caretCard.match(/<div class="section-header floating-toolbar-setting-row caret-state-heading">([\s\S]*?)<\/div>\s*<\/div>\s*<\/div>/)?.[1] ?? '';
  expect(heading).toContain('<div class="section-title">光标状态提示</div>');
  // The group title has no switch of its own.
  expect(heading).not.toContain('ftb-toggle-btn');
  // Only the group title keeps the plain bold section title; the rows under
  // it share the section-title size and colour with a lighter weight.
  const groupTitles = Array.from(caretCard.matchAll(/<div class="section-title">([^<]+)<\/div>/g), ([, title]) => title);
  expect(groupTitles).toEqual(['光标状态提示']);
  const rowTitles = Array.from(
    caretCard.matchAll(/<div class="section-title caret-state-option-title">([^<]+)<\/div>/g), ([, title]) => title);
  expect(rowTitles).toEqual(['切换输入法状态时提示', '切换输入框时提示', '提示位置']);
  expect(styles).toMatch(/\.caret-state-heading\s*\{[^}]*border-bottom:/);
  expect(styles).toMatch(/\.section-title\.caret-state-option-title\s*\{[^}]*font-weight:\s*400;[^}]*-webkit-text-stroke:/);
});

it('shows the four runtime samples and preserves fixed punctuation slots', () => {
  const preview = partial.match(/<div class="candidate caret-state-preview-host"[\s\S]*?<\/div>\s*<\/div>\s*<\/div>/)?.[0] ?? '';
  expect(preview).toContain('>中</div>');
  expect(preview).toContain('aria-label="，。  中"');
  expect(preview).toContain('>全</div>');
  expect(preview).toContain('>简</div>');
  expect(preview.match(/class="caret-state-badge(?: |")/g)).toHaveLength(4);
  expect(preview.match(/class="caret-state-preview-item"[^>]*>\s*<div class="caret-state-badge[^>]*>[\s\S]*?<\/div>\s*<span class="caret-state-preview-caret"><\/span>\s*<\/div>/g)).toHaveLength(4);
  expect(preview).toContain('data-position="top-left"');
  expect(styles).toMatch(/\.caret-state-badge\s*\{[^}]*width:\s*30px;[^}]*height:\s*30px;/);
  expect(styles).toMatch(/\.caret-state-badge-punctuation\s*\{[^}]*display:\s*block;[^}]*width:\s*94px;/);
  expect(styles).toMatch(/\.caret-state-badge-punctuation\s*\{\s*--badge-width:\s*94px;/);
  expect(styles).toMatch(/\.caret-state-punctuation-slot\s*\{[^}]*left:\s*-1px;[^}]*width:\s*64px;/);
  expect(styles).toMatch(/\.caret-state-mode-slot\s*\{[^}]*right:\s*-1px;[^}]*width:\s*30px;/);
  expect(styles).toMatch(/\.caret-state-preview-host\s*\{[^}]*font-size:\s*20px;/);
  expect(styles).toMatch(/\.caret-state-preview-host\s*\{[^}]*grid-template-columns:\s*repeat\(4, 132px\);/);
  expect(styles).toMatch(/@container\s*\(max-width:\s*569px\)\s*\{\s*\.caret-state-preview-host\s*\{[^}]*grid-template-columns:\s*repeat\(2, 132px\);/);
  expect(styles).toMatch(/\.caret-state-preview-item\s*\{[^}]*width:\s*132px;[^}]*height:\s*132px;[^}]*border:/);
  expect(styles).toMatch(/\.caret-state-preview-caret\s*\{[^}]*width:\s*2px;[^}]*height:\s*24px;/);
  expect(styles).toMatch(/\.caret-state-badge\s*\{[^}]*top:\s*23px;[^}]*right:\s*29px;/);
  expect(styles).toMatch(/\[data-position="top"\] \.caret-state-badge/);
  expect(styles).toMatch(/\[data-position="top-right"\] \.caret-state-badge/);
  // Every lower position shares one vertical offset; bottom centres like top
  // and bottom-right aligns like top-right (bottom-left keeps the default).
  expect(styles).toMatch(/\[data-position\^="bottom"\] \.caret-state-badge\s*\{\s*top:\s*89px;/);
  expect(styles).toMatch(/\[data-position="bottom"\] \.caret-state-badge\s*\{\s*right:\s*auto;\s*left:\s*calc\(65px/);
  expect(styles).toMatch(/\[data-position="bottom-right"\] \.caret-state-badge\s*\{\s*right:\s*auto;\s*left:\s*23px;/);
  expect(styles).not.toMatch(/\[data-position="bottom-left"\]/);
});

it('offers every side and alignment in the position selector', () => {
  const menu = partial.split('id="caretStateIndicatorPositionMenu"')[1]?.split('</button>')[0] ?? '';
  const options = Array.from(menu.matchAll(/data-value="([^"]+)">([^<]+)<\/div>/g), ([, value, label]) => [value, label]);
  expect(options).toEqual([
    ['top-left', '左上方'], ['top', '正上方'], ['top-right', '右上方'],
    ['bottom-left', '左下方'], ['bottom', '正下方'], ['bottom-right', '右下方']
  ]);
});

it('applies each selector choice immediately to all samples and also follows config snapshots', () => {
  const label = { setAttribute: vi.fn() };
  const host = { dataset: { position: 'top-left' }, closest: () => label };
  vi.stubGlobal('document', {
    getElementById: (id: string) => id === 'caretStatePreviewHost' ? host : null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    const call = vi.mocked(setupDropdownMenu).mock.calls.find(([button]) => button === 'caretStateIndicatorPositionBtn');
    expect(call?.[4]).toBe('general.caret_state_indicator_position');
    const change = call?.[5];
    for (const [position, direction] of [
      ['top-left', '左上方'], ['top', '正上方'], ['top-right', '右上方'],
      ['bottom-left', '左下方'], ['bottom', '正下方'], ['bottom-right', '右下方']
    ]) {
      expect(change?.(position)).toBe(position);
      expect(host.dataset.position).toBe(position);
      expect(label.setAttribute).toHaveBeenLastCalledWith('aria-label', expect.stringContaining(`每个文字光标${direction}`));
      applyCaretStateIndicatorPosition(position);
      expect(applyDropdownValue).toHaveBeenLastCalledWith('caretStateIndicatorPositionBtn', 'caretStateIndicatorPositionMenu', position);
      expect(host.dataset.position).toBe(position);
    }
  } finally {
    vi.unstubAllGlobals();
  }
});

it('persists the focus announcement switch under its own config key', () => {
  const toggle = { setAttribute: vi.fn() };
  vi.stubGlobal('document', {
    getElementById: (id: string) => id === 'caretStateIndicatorOnFocusToggleBtn' ? toggle : null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    const call = vi.mocked(setupToggleButton).mock.calls.find(([id]) => id === 'caretStateIndicatorOnFocusToggleBtn');
    const onToggle = call?.[1];
    expect(onToggle).toBeTypeOf('function');
    onToggle!(true);
    expect(updateConfig).toHaveBeenLastCalledWith('general.caret_state_indicator_on_focus', true);
    expect(toggle.setAttribute).toHaveBeenLastCalledWith('aria-checked', 'true');
    onToggle!(false);
    expect(updateConfig).toHaveBeenLastCalledWith('general.caret_state_indicator_on_focus', false);
  } finally {
    vi.unstubAllGlobals();
  }
});

it('puts the auto-hide switch and its delay stepper in the toolbar card', () => {
  const toolbarCard = partial.match(/<div class="section floating-toolbar-card">([\s\S]*?)<div class="section caret-state-indicator-card">/)?.[1] ?? '';
  expect(toolbarCard).toContain('class="ftb-toggle-btn" id="ftbAutoHideToggleBtn" role="switch" aria-label="自动隐藏工具栏" aria-checked="false"');
  expect(toolbarCard).toContain('id="ftbAutoHideDelayDecBtn"');
  expect(toolbarCard).toContain('id="ftbAutoHideDelayInput"');
  expect(toolbarCard).toContain('id="ftbAutoHideDelayIncBtn"');
  // The delay row starts greyed out, matching the switch's default of off.
  expect(toolbarCard).toMatch(/class="[^"]*ftb-auto-hide-delay-row is-disabled" id="ftbAutoHideDelayRow"/);
  expect(toolbarCard.indexOf('id="ftbAutoHideToggleBtn"')).toBeLessThan(toolbarCard.indexOf('id="ftbPreviewHost"'));
});

it('clamps the auto-hide delay to the server range', () => {
  expect(clampAutoHideDelay(0)).toBe(1);
  expect(clampAutoHideDelay(5)).toBe(5);
  expect(clampAutoHideDelay(60)).toBe(60);
  expect(clampAutoHideDelay(61)).toBe(60);
  expect(clampAutoHideDelay(2.6)).toBe(3);
  expect(clampAutoHideDelay(Number.NaN)).toBe(5);
});

it('clamps the auto-dim delay to the server range', () => {
  expect(clampAutoDimDelay(0)).toBe(1);
  expect(clampAutoDimDelay(5)).toBe(5);
  expect(clampAutoDimDelay(60)).toBe(60);
  expect(clampAutoDimDelay(61)).toBe(60);
  expect(clampAutoDimDelay(2.6)).toBe(3);
  expect(clampAutoDimDelay(Number.NaN)).toBe(5);
});

it('puts the opacity steppers and the auto-dim rows under the main switch', () => {
  const toolbarCard = partial.match(/<div class="section floating-toolbar-card">([\s\S]*?)<div class="section caret-state-indicator-card">/)?.[1] ?? '';
  // 背景/前景不透明度紧跟主开关，排在自动隐藏之前；副标题逐字对应需求描述。
  expect(toolbarCard.indexOf('id="ftbBackgroundOpacityDecBtn"')).toBeGreaterThan(toolbarCard.indexOf('id="ftbToggleBtn"'));
  expect(toolbarCard.indexOf('id="ftbBackgroundOpacityDecBtn"')).toBeLessThan(toolbarCard.indexOf('id="ftbAutoHideToggleBtn"'));
  expect(toolbarCard).toContain('工具栏显示一段时间无交互后降低不透明度；鼠标悬停时不降低，切换中英等状态变化时回到原不透明度。');
  // 五个不透明度相关的选项都是 −/+ 步进器，与延时选项同款；两个自动降低的
  // 子选项初始置灰、缩进。
  for (const id of ['ftbBackgroundOpacityDecBtn', 'ftbBackgroundOpacityInput', 'ftbBackgroundOpacityIncBtn',
                    'ftbForegroundOpacityDecBtn', 'ftbForegroundOpacityInput', 'ftbForegroundOpacityIncBtn']) {
    expect(toolbarCard).toContain(`id="${id}"`);
  }
  // 背景在前、前景在后，两者都排在自动降低之前。
  expect(toolbarCard.indexOf('id="ftbBackgroundOpacityDecBtn"')).toBeLessThan(
    toolbarCard.indexOf('id="ftbForegroundOpacityDecBtn"'));
  expect(toolbarCard.indexOf('id="ftbForegroundOpacityIncBtn"')).toBeLessThan(
    toolbarCard.indexOf('id="ftbAutoDimToggleBtn"'));
  expect(toolbarCard).toMatch(/class="[^"]*ftb-auto-dim-opacity-row is-disabled" id="ftbAutoDimOpacityRow"/);
  expect(toolbarCard).toMatch(/class="[^"]*ftb-auto-dim-delay-row is-disabled" id="ftbAutoDimDelayRow"/);
  for (const id of ['ftbAutoDimOpacityDecBtn', 'ftbAutoDimOpacityInput', 'ftbAutoDimOpacityIncBtn']) {
    expect(toolbarCard).toContain(`id="${id}"`);
  }
  expect(toolbarCard).toContain('id="ftbAutoDimDelayDecBtn"');
  expect(toolbarCard).toContain('id="ftbAutoDimDelayInput"');
  expect(toolbarCard).toContain('id="ftbAutoDimDelayIncBtn"');
  // WebView2 渲染下只显示的那一个（初始收起，出厂默认是 D2D），与「背景不透明度」同一个键。
  expect(toolbarCard).toContain('id="ftbOpacitySplitRows"');
  expect(toolbarCard).toMatch(/id="ftbOpacitySingleRow"\s+hidden/);
  for (const id of ['ftbOpacityDecBtn', 'ftbOpacityInput', 'ftbOpacityIncBtn']) {
    expect(toolbarCard).toContain(`id="${id}"`);
  }
  const splitRows = toolbarCard.match(/id="ftbOpacitySplitRows"([\s\S]*?)id="ftbOpacitySingleRow"/)?.[1] ?? '';
  expect(splitRows).toContain('id="ftbBackgroundOpacityDecBtn"');
  expect(splitRows).toContain('id="ftbForegroundOpacityDecBtn"');
  expect(toolbarCard.indexOf('id="ftbAutoDimToggleBtn"')).toBeLessThan(toolbarCard.indexOf('id="ftbAutoDimOpacityRow"'));
  expect(toolbarCard.indexOf('id="ftbAutoDimOpacityRow"')).toBeLessThan(toolbarCard.indexOf('id="ftbAutoDimDelayRow"'));
  expect(styles).toMatch(/\.ftb-auto-dim-delay-row\s*\{[^}]*padding-left:\s*48px;/);
  expect(styles).toMatch(/\.ftb-auto-dim-delay-row\.is-disabled \.number-stepper\s*\{[^}]*pointer-events:\s*none;/);
  // 分组选择器里这一项后面跟的是逗号，用 [,{] 同时容忍两种写法。
  expect(styles).toMatch(/\.ftb-auto-dim-opacity-row\.is-disabled \.number-stepper\s*[,{]/);
});

it('steps the toolbar background and foreground opacity within 1–100 and persists each change', () => {
  const listeners = new Map<string, () => void>();
  const element = (id: string) => ({
    id,
    value: '',
    disabled: false,
    tabIndex: 0,
    setAttribute: vi.fn(),
    classList: { toggle: vi.fn() },
    addEventListener: (type: string, listener: () => void) => { if (type === 'click') listeners.set(id, listener); }
  });
  const elements = new Map(
    ['ftbBackgroundOpacityDecBtn', 'ftbBackgroundOpacityInput', 'ftbBackgroundOpacityIncBtn',
     'ftbForegroundOpacityDecBtn', 'ftbForegroundOpacityInput', 'ftbForegroundOpacityIncBtn',
     'ftbAutoDimOpacityDecBtn', 'ftbAutoDimOpacityInput', 'ftbAutoDimOpacityIncBtn']
      .map((id) => [id, element(id)]));
  vi.stubGlobal('document', {
    getElementById: (id: string) => elements.get(id) ?? null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    // 快照回填一个中间值：± 的禁用态按区间刷新。
    applyFloatingToolbarAppearanceConfig(undefined, undefined, 70, 40);
    expect(elements.get('ftbBackgroundOpacityInput')!.value).toBe('70');
    expect(elements.get('ftbForegroundOpacityInput')!.value).toBe('40');
    expect(elements.get('ftbBackgroundOpacityDecBtn')!.disabled).toBe(false);
    expect(elements.get('ftbBackgroundOpacityIncBtn')!.disabled).toBe(false);

    listeners.get('ftbBackgroundOpacityIncBtn')!();
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_background_opacity', 71);
    expect(elements.get('ftbBackgroundOpacityInput')!.value).toBe('71');

    // 前景走自己的配置键。
    listeners.get('ftbForegroundOpacityIncBtn')!();
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_foreground_opacity', 41);

    // 无交互后的不透明度也走自己的配置键。
    applyFloatingToolbarAutoDimConfig(undefined, 5, undefined);
    expect(elements.get('ftbAutoDimOpacityInput')!.value).toBe('5');
    listeners.get('ftbAutoDimOpacityIncBtn')!();
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_auto_dim_opacity', 6);

    // 下限再按 −：值不变，不上报。
    vi.mocked(updateConfig).mockClear();
    applyFloatingToolbarAppearanceConfig(undefined, undefined, 1, 1);
    expect(elements.get('ftbBackgroundOpacityDecBtn')!.disabled).toBe(true);
    listeners.get('ftbBackgroundOpacityDecBtn')!();
    expect(updateConfig).not.toHaveBeenCalled();
    expect(elements.get('ftbBackgroundOpacityInput')!.value).toBe('1');
    listeners.get('ftbForegroundOpacityDecBtn')!();
    expect(updateConfig).not.toHaveBeenCalled();

    // 上限再按 +：同样不上报。
    applyFloatingToolbarAppearanceConfig(undefined, undefined, 100, 100);
    expect(elements.get('ftbBackgroundOpacityIncBtn')!.disabled).toBe(true);
    listeners.get('ftbBackgroundOpacityIncBtn')!();
    expect(updateConfig).not.toHaveBeenCalled();
  } finally {
    vi.unstubAllGlobals();
  }
});

it('shows one toolbar opacity option under WebView2 and the foreground/background pair under D2D', () => {
  const listeners = new Map<string, () => void>();
  const element = (id: string) => ({
    id,
    value: '',
    disabled: false,
    hidden: false,
    tabIndex: 0,
    setAttribute: vi.fn(),
    classList: { toggle: vi.fn() },
    addEventListener: (type: string, listener: () => void) => { if (type === 'click') listeners.set(id, listener); }
  });
  const elements = new Map(
    ['ftbOpacitySplitRows', 'ftbOpacitySingleRow', 'ftbOpacityDecBtn', 'ftbOpacityInput', 'ftbOpacityIncBtn']
      .map((id) => [id, element(id)]));
  elements.get('ftbOpacitySingleRow')!.hidden = true;
  vi.stubGlobal('document', {
    getElementById: (id: string) => elements.get(id) ?? null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    // D2D：前景/背景各一个，单选项那块收起来，值跟「背景不透明度」同步。
    applyFloatingToolbarAppearanceConfig(undefined, undefined, 70, 40, 'd2d');
    expect(elements.get('ftbOpacitySplitRows')!.hidden).toBe(false);
    expect(elements.get('ftbOpacitySingleRow')!.hidden).toBe(true);
    expect(elements.get('ftbOpacityInput')!.value).toBe('70');

    // WebView2：宿主只有整窗一个 alpha，前景那个对它无效，整块收成一个选项。
    applyFloatingToolbarAppearanceConfig(undefined, undefined, 70, 40, 'webview2');
    expect(elements.get('ftbOpacitySplitRows')!.hidden).toBe(true);
    expect(elements.get('ftbOpacitySingleRow')!.hidden).toBe(false);

    // 那一个选项绑的就是「背景不透明度」这个键：WebView2 的整窗 alpha 取的就是它。
    listeners.get('ftbOpacityIncBtn')!();
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_background_opacity', 71);

    // 快照没带 ui_backend 时沿用上次的值，不回退。
    applyFloatingToolbarAppearanceConfig(undefined, undefined, 70, 40);
    expect(elements.get('ftbOpacitySplitRows')!.hidden).toBe(true);
  } finally {
    vi.unstubAllGlobals();
  }
});

it('persists the auto-dim switch, greys its sub-options and steps the delay within range', () => {
  const listeners = new Map<string, () => void>();
  const element = (id: string) => ({
    id,
    value: '',
    disabled: false,
    tabIndex: 0,
    setAttribute: vi.fn(),
    classList: { toggle: vi.fn() },
    addEventListener: (type: string, listener: () => void) => { if (type === 'click') listeners.set(id, listener); }
  });
  const elements = new Map(
    ['ftbAutoDimToggleBtn', 'ftbAutoDimOpacityRow', 'ftbAutoDimDelayRow',
     'ftbAutoDimOpacityDecBtn', 'ftbAutoDimOpacityInput', 'ftbAutoDimOpacityIncBtn',
     'ftbAutoDimDelayDecBtn', 'ftbAutoDimDelayInput', 'ftbAutoDimDelayIncBtn']
      .map((id) => [id, element(id)]));
  vi.stubGlobal('document', {
    getElementById: (id: string) => elements.get(id) ?? null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    const onToggle = vi.mocked(setupToggleButton).mock.calls.find(([id]) => id === 'ftbAutoDimToggleBtn')?.[1];
    onToggle!(true);
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_auto_dim', true);
    expect(elements.get('ftbAutoDimOpacityRow')!.classList.toggle).toHaveBeenLastCalledWith('is-disabled', false);
    expect(elements.get('ftbAutoDimDelayRow')!.classList.toggle).toHaveBeenLastCalledWith('is-disabled', false);
    // 两个子选项的控件一并恢复 Tab。
    expect(elements.get('ftbAutoDimDelayDecBtn')!.tabIndex).toBe(0);
    expect(elements.get('ftbAutoDimOpacityInput')!.tabIndex).toBe(0);

    onToggle!(false);
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_auto_dim', false);
    expect(elements.get('ftbAutoDimDelayRow')!.classList.toggle).toHaveBeenLastCalledWith('is-disabled', true);
    expect(elements.get('ftbAutoDimDelayInput')!.tabIndex).toBe(-1);
    expect(elements.get('ftbAutoDimOpacityIncBtn')!.tabIndex).toBe(-1);

    // 快照回填：开关、无交互后的不透明度、延时各自落到自己的控件上。
    vi.mocked(updateConfig).mockClear();
    applyFloatingToolbarAutoDimConfig(true, 30, 12);
    expect(elements.get('ftbAutoDimOpacityInput')!.value).toBe('30');
    expect(elements.get('ftbAutoDimDelayInput')!.value).toBe('12');
    expect(updateConfig).not.toHaveBeenCalled();

    listeners.get('ftbAutoDimDelayIncBtn')!();
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_auto_dim_delay', 13);
    expect(elements.get('ftbAutoDimDelayIncBtn')!.disabled).toBe(false);
    listeners.get('ftbAutoDimDelayDecBtn')!();
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_auto_dim_delay', 12);
  } finally {
    vi.unstubAllGlobals();
  }
});

it('persists the auto-hide switch and steps the delay within range', () => {
  const listeners = new Map<string, () => void>();
  const element = (id: string) => ({
    id,
    value: '',
    disabled: false,
    tabIndex: 0,
    setAttribute: vi.fn(),
    classList: { toggle: vi.fn() },
    addEventListener: (type: string, listener: () => void) => { if (type === 'click') listeners.set(id, listener); }
  });
  const elements = new Map(
    ['ftbAutoHideToggleBtn', 'ftbAutoHideDelayRow', 'ftbAutoHideDelayDecBtn', 'ftbAutoHideDelayInput', 'ftbAutoHideDelayIncBtn']
      .map((id) => [id, element(id)]));
  vi.stubGlobal('document', {
    getElementById: (id: string) => elements.get(id) ?? null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    const onToggle = vi.mocked(setupToggleButton).mock.calls.find(([id]) => id === 'ftbAutoHideToggleBtn')?.[1];
    onToggle!(true);
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_auto_hide', true);
    expect(elements.get('ftbAutoHideDelayRow')!.classList.toggle).toHaveBeenLastCalledWith('is-disabled', false);

    applyFloatingToolbarAutoHideConfig(true, 59);
    expect(elements.get('ftbAutoHideDelayInput')!.value).toBe('59');
    listeners.get('ftbAutoHideDelayIncBtn')!();
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_auto_hide_delay', 60);
    expect(elements.get('ftbAutoHideDelayIncBtn')!.disabled).toBe(true);
    vi.mocked(updateConfig).mockClear();
    listeners.get('ftbAutoHideDelayIncBtn')!();
    expect(updateConfig).not.toHaveBeenCalled();

    applyFloatingToolbarAutoHideConfig(true, 1);
    listeners.get('ftbAutoHideDelayDecBtn')!();
    expect(updateConfig).not.toHaveBeenCalled();
    listeners.get('ftbAutoHideDelayIncBtn')!();
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_auto_hide_delay', 2);
  } finally {
    vi.unstubAllGlobals();
  }
});

it('persists the shadow switch and mirrors it on the preview', () => {
  const toolbarCard = partial.match(/<div class="section floating-toolbar-card">([\s\S]*?)<div class="section caret-state-indicator-card">/)?.[1] ?? '';
  expect(toolbarCard).toContain('class="ftb-toggle-btn active" id="ftbShadowToggleBtn" role="switch" aria-label="工具栏阴影" aria-checked="true"');
  expect(styles).toMatch(/\.ftb-preview-host\.no-shadow \.status-bar\s*\{\s*box-shadow:\s*none !important;/);

  const toggle = { setAttribute: vi.fn() };
  const preview = { classList: { toggle: vi.fn() } };
  // Withheld during setup so mounting the preview (DOMParser) is skipped.
  let previewMounted = false;
  vi.stubGlobal('document', {
    getElementById: (id: string) => id === 'ftbShadowToggleBtn' ? toggle
      : id === 'ftbPreviewHost' && previewMounted ? preview : null,
    querySelectorAll: () => []
  });
  try {
    setupFloatingToolbar();
    previewMounted = true;
    const onToggle = vi.mocked(setupToggleButton).mock.calls.find(([id]) => id === 'ftbShadowToggleBtn')?.[1];
    onToggle!(false);
    expect(updateConfig).toHaveBeenLastCalledWith('general.floating_toolbar_shadow', false);
    expect(preview.classList.toggle).toHaveBeenLastCalledWith('no-shadow', true);

    applyFloatingToolbarShadowConfig(true);
    expect(applyToggleState).toHaveBeenLastCalledWith('ftbShadowToggleBtn', true);
    expect(preview.classList.toggle).toHaveBeenLastCalledWith('no-shadow', false);
  } finally {
    vi.unstubAllGlobals();
  }
});