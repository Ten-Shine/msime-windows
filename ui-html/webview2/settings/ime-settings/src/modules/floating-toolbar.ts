import {
  applyDropdownValue,
  applyToggleState,
  isWebView2UiBackend,
  setupDropdownMenu,
  setupToggleButton
} from './shared';
import {
  clampNumberStepperValue,
  renderNumberStepper,
  setNumberStepperTabIndex,
  setupNumberStepper,
  type NumberStepperIds
} from './number-stepper';
import { syncCaretStateIndicatorPreview } from './appearance';
import { updateConfig } from './config-sync';
import { syncAppearancePreviews } from './skin';
import { applyToolbarIconGlyphFallbacks } from './toolbar-icon-glyphs';
import ftbHTML from '../../../../ftb/default.html?raw';

type FloatingToolbarItem = 'fullwidth' | 'punctuation' | 'character_set' | 'emoji' | 'screen_keyboard' | 'settings';
type FloatingToolbarItemsConfig = Partial<Record<FloatingToolbarItem, boolean>>;

const toolbarItems: FloatingToolbarItem[] = [
  'fullwidth',
  'punctuation',
  'character_set',
  'emoji',
  'screen_keyboard',
  'settings'
];

const toolbarItemState: Record<FloatingToolbarItem, boolean> = {
  fullwidth: true,
  punctuation: true,
  character_set: true,
  emoji: true,
  screen_keyboard: false,
  settings: true
};

let toolbarScale = 1;
let toolbarFontSize = 24;
let toolbarBackgroundOpacity = 100;
let toolbarForegroundOpacity = 100;
let toolbarAutoDimOpacity = 50;
// 与候选窗同一条：ui_backend 决定不透明度是拆成前景/背景，还是只剩一个。
let uiBackend = 'd2d';

// Mirrors kWindowOpacityPercent{Min,Max,Default} in the server config.
const OPACITY_MIN = 1;
const OPACITY_MAX = 100;
const OPACITY_DEFAULT = 100;
const TOOLBAR_BACKGROUND_OPACITY_IDS: NumberStepperIds = {
  dec: 'ftbBackgroundOpacityDecBtn',
  input: 'ftbBackgroundOpacityInput',
  inc: 'ftbBackgroundOpacityIncBtn'
};
const TOOLBAR_FOREGROUND_OPACITY_IDS: NumberStepperIds = {
  dec: 'ftbForegroundOpacityDecBtn',
  input: 'ftbForegroundOpacityInput',
  inc: 'ftbForegroundOpacityIncBtn'
};
// WebView2 渲染下的唯一一个不透明度选项。宿主整窗 alpha 取的就是
// floating_toolbar_background_opacity（见 FloatingToolbarBaseAlpha），所以这一个选项
// 直接绑那个键——不新增配置项，C++ 侧一行都不用动。与「背景不透明度」共用一份状态。
const TOOLBAR_OPACITY_IDS: NumberStepperIds = {
  dec: 'ftbOpacityDecBtn',
  input: 'ftbOpacityInput',
  inc: 'ftbOpacityIncBtn'
};
const TOOLBAR_DIM_OPACITY_IDS: NumberStepperIds = {
  dec: 'ftbAutoDimOpacityDecBtn',
  input: 'ftbAutoDimOpacityInput',
  inc: 'ftbAutoDimOpacityIncBtn'
};

// Mirrors kFloatingToolbarAutoHideDelay{Min,Max,Default} in the server config.
export const AUTO_HIDE_DELAY_MIN = 1;
export const AUTO_HIDE_DELAY_MAX = 60;
const AUTO_HIDE_DELAY_DEFAULT = 5;
const AUTO_HIDE_DELAY_IDS: NumberStepperIds = {
  dec: 'ftbAutoHideDelayDecBtn',
  input: 'ftbAutoHideDelayInput',
  inc: 'ftbAutoHideDelayIncBtn'
};
let autoHideDelay = AUTO_HIDE_DELAY_DEFAULT;

// Mirrors kFloatingToolbarAutoDimDelay{Min,Max,Default} in the server config.
export const AUTO_DIM_DELAY_MIN = 1;
export const AUTO_DIM_DELAY_MAX = 60;
const AUTO_DIM_DELAY_DEFAULT = 5;
const AUTO_DIM_DELAY_IDS: NumberStepperIds = {
  dec: 'ftbAutoDimDelayDecBtn',
  input: 'ftbAutoDimDelayInput',
  inc: 'ftbAutoDimDelayIncBtn'
};
let autoDimDelay = AUTO_DIM_DELAY_DEFAULT;

export function setupFloatingToolbar(): void {
  mountFloatingToolbarPreview();
  syncCaretStateIndicatorPreview();
  syncAppearancePreviews();

  setupToggleButton('ftbToggleBtn', (active) => {
    updateConfig('general.floating_toolbar', active);
    document.getElementById('ftbToggleBtn')?.setAttribute('aria-checked', String(active));
  });

  // 工具栏背景/前景不透明度：−/+ 步进器，1–100 的整数。前景只在 D2D 渲染下
  // 生效（WebView2 端整窗一个 alpha，取背景不透明度）。
  setupNumberStepper(TOOLBAR_BACKGROUND_OPACITY_IDS, {
    min: OPACITY_MIN,
    max: OPACITY_MAX,
    fallback: OPACITY_DEFAULT,
    getValue: () => toolbarBackgroundOpacity,
    onCommit: (value) => {
      toolbarBackgroundOpacity = value;
      applyPreviewAppearance();
      updateConfig('general.floating_toolbar_background_opacity', value);
    }
  });
  setupNumberStepper(TOOLBAR_FOREGROUND_OPACITY_IDS, {
    min: OPACITY_MIN,
    max: OPACITY_MAX,
    fallback: OPACITY_DEFAULT,
    getValue: () => toolbarForegroundOpacity,
    onCommit: (value) => {
      toolbarForegroundOpacity = value;
      updateConfig('general.floating_toolbar_foreground_opacity', value);
    }
  });
  // WebView2 渲染下的那一个：与「背景不透明度」是同一个配置键、同一份状态，只是换了
  // 个名字——那边整窗就一个 alpha，叫"背景"没有意义。
  setupNumberStepper(TOOLBAR_OPACITY_IDS, {
    min: OPACITY_MIN,
    max: OPACITY_MAX,
    fallback: OPACITY_DEFAULT,
    getValue: () => toolbarBackgroundOpacity,
    onCommit: (value) => {
      toolbarBackgroundOpacity = value;
      applyPreviewAppearance();
      updateConfig('general.floating_toolbar_background_opacity', value);
    }
  });

  setupToggleButton('ftbAutoDimToggleBtn', (active) => {
    updateConfig('general.floating_toolbar_auto_dim', active);
    document.getElementById('ftbAutoDimToggleBtn')?.setAttribute('aria-checked', String(active));
    setAutoDimOptionsDisabled(!active);
  });

  setupNumberStepper(TOOLBAR_DIM_OPACITY_IDS, {
    min: OPACITY_MIN,
    max: OPACITY_MAX,
    fallback: 50,
    getValue: () => toolbarAutoDimOpacity,
    onCommit: (value) => {
      toolbarAutoDimOpacity = value;
      applyPreviewAppearance();
      updateConfig('general.floating_toolbar_auto_dim_opacity', value);
    }
  });
  setupNumberStepper(AUTO_DIM_DELAY_IDS, {
    min: AUTO_DIM_DELAY_MIN,
    max: AUTO_DIM_DELAY_MAX,
    fallback: AUTO_DIM_DELAY_DEFAULT,
    getValue: () => autoDimDelay,
    onCommit: (value) => {
      autoDimDelay = value;
      updateConfig('general.floating_toolbar_auto_dim_delay', value);
    }
  });

  setupToggleButton('ftbAutoHideToggleBtn', (active) => {
    updateConfig('general.floating_toolbar_auto_hide', active);
    document.getElementById('ftbAutoHideToggleBtn')?.setAttribute('aria-checked', String(active));
    setAutoHideDelayDisabled(!active);
  });
  setupNumberStepper(AUTO_HIDE_DELAY_IDS, {
    min: AUTO_HIDE_DELAY_MIN,
    max: AUTO_HIDE_DELAY_MAX,
    fallback: AUTO_HIDE_DELAY_DEFAULT,
    getValue: () => autoHideDelay,
    onCommit: (value) => {
      autoHideDelay = value;
      updateConfig('general.floating_toolbar_auto_hide_delay', value);
    }
  });

  setupToggleButton('ftbShadowToggleBtn', (active) => {
    updateConfig('general.floating_toolbar_shadow', active);
    document.getElementById('ftbShadowToggleBtn')?.setAttribute('aria-checked', String(active));
    setPreviewShadow(active);
  });

  setupToggleButton('caretStateIndicatorToggleBtn', (active) => {
    updateConfig('general.caret_state_indicator', active);
    document.getElementById('caretStateIndicatorToggleBtn')?.setAttribute('aria-checked', String(active));
  });

  setupToggleButton('caretStateIndicatorOnFocusToggleBtn', (active) => {
    updateConfig('general.caret_state_indicator_on_focus', active);
    document.getElementById('caretStateIndicatorOnFocusToggleBtn')?.setAttribute('aria-checked', String(active));
  });

  setupDropdownMenu('caretStateIndicatorPositionBtn', 'caretStateIndicatorPositionMenu', '', true,
    'general.caret_state_indicator_position', (value) => {
      updateCaretPreviewPosition(value);
      return value;
    });

  setupDropdownMenu('ftbScaleBtn', 'ftbScaleMenu', '', true, 'general.floating_toolbar_scale', (value) => {
    const parsed = Number(value);
    toolbarScale = Number.isFinite(parsed) && parsed > 0 ? parsed : 1;
    applyPreviewAppearance();
    return toolbarScale;
  });

  setupDropdownMenu('ftbFontSizeBtn', 'ftbFontSizeMenu', '', true, 'general.floating_toolbar_font_size', (value) => {
    const parsed = Number(value);
    toolbarFontSize = Number.isFinite(parsed) ? parsed : 24;
    applyPreviewAppearance();
    return toolbarFontSize;
  });

  document.querySelectorAll<HTMLInputElement>('.floating-toolbar-component-list input[data-toolbar-item]')
    .forEach((checkbox) => {
      checkbox.addEventListener('change', () => {
        const item = checkbox.dataset.toolbarItem as FloatingToolbarItem;
        toolbarItemState[item] = checkbox.checked;
        updatePreviewItems();
        updateConfig(`general.floating_toolbar_${item}`, checkbox.checked);
      });
    });
}

export function applyFloatingToolbarItemsConfig(config: FloatingToolbarItemsConfig | undefined): void {
  if (!config) return;
  toolbarItems.forEach((item) => {
    if (typeof config[item] !== 'boolean') return;
    toolbarItemState[item] = config[item]!;
    const checkbox = document.querySelector<HTMLInputElement>(
      `.floating-toolbar-component-list input[data-toolbar-item="${item}"]`
    );
    if (checkbox) checkbox.checked = toolbarItemState[item];
  });
  updatePreviewItems();
}

export function applyCaretStateIndicatorPosition(position?: string): void {
  const value = position || 'top-left';
  applyDropdownValue('caretStateIndicatorPositionBtn', 'caretStateIndicatorPositionMenu', value);
  updateCaretPreviewPosition(value);
}

function updateCaretPreviewPosition(position: string): void {
  const host = document.getElementById('caretStatePreviewHost');
  if (!host) return;
  host.dataset.position = position;
  const direction = ({
    'top-left': '左上方', top: '正上方', 'top-right': '右上方',
    'bottom-left': '左下方', bottom: '正下方', 'bottom-right': '右下方'
  } as Record<string, string>)[position];
  host.closest('.caret-state-preview')?.setAttribute('aria-label',
    `光标状态提示预览：每个文字光标${direction || '左上方'}分别显示中、中文标点和中文模式、全角、简体`);
}

export function applyFloatingToolbarAppearanceConfig(
  scale?: number,
  fontSize?: number,
  backgroundOpacity?: number,
  foregroundOpacity?: number,
  backend?: string
): void {
  if (typeof scale === 'number' && scale > 0) {
    toolbarScale = scale;
    applyDropdownValue('ftbScaleBtn', 'ftbScaleMenu', normalizeScaleKey(scale));
  }
  if (typeof fontSize === 'number' && fontSize > 0) {
    toolbarFontSize = fontSize;
    applyDropdownValue('ftbFontSizeBtn', 'ftbFontSizeMenu', String(fontSize));
  }
  if (typeof backgroundOpacity === 'number') {
    toolbarBackgroundOpacity = clampNumberStepperValue(backgroundOpacity, OPACITY_MIN, OPACITY_MAX, OPACITY_DEFAULT);
    renderNumberStepper(TOOLBAR_BACKGROUND_OPACITY_IDS, toolbarBackgroundOpacity, OPACITY_MIN, OPACITY_MAX);
  }
  if (typeof foregroundOpacity === 'number') {
    toolbarForegroundOpacity = clampNumberStepperValue(foregroundOpacity, OPACITY_MIN, OPACITY_MAX, OPACITY_DEFAULT);
    renderNumberStepper(TOOLBAR_FOREGROUND_OPACITY_IDS, toolbarForegroundOpacity, OPACITY_MIN, OPACITY_MAX);
  }
  applyFloatingToolbarOpacityRows(backend);
  applyPreviewAppearance();
}

// 同候选窗：WebView2 渲染下宿主只有整窗一个 alpha，前景不透明度对它无效，所以工具栏
// 那边也只留一个不透明度选项。backend 省略时沿用上次的值。
export function applyFloatingToolbarOpacityRows(backend?: string): void {
  if (backend) {
    uiBackend = backend;
  }
  const webview2 = isWebView2UiBackend(uiBackend);
  const splitRows = document.getElementById('ftbOpacitySplitRows');
  if (splitRows) splitRows.hidden = webview2;
  const singleRow = document.getElementById('ftbOpacitySingleRow');
  if (singleRow) singleRow.hidden = !webview2;
  renderNumberStepper(TOOLBAR_OPACITY_IDS, toolbarBackgroundOpacity, OPACITY_MIN, OPACITY_MAX);
}

export function clampAutoDimDelay(value: number): number {
  if (!Number.isFinite(value)) return AUTO_DIM_DELAY_DEFAULT;
  return Math.min(AUTO_DIM_DELAY_MAX, Math.max(AUTO_DIM_DELAY_MIN, Math.round(value)));
}

export function applyFloatingToolbarAutoDimConfig(enabled?: boolean, dimOpacity?: number, delay?: number): void {
  if (typeof enabled === 'boolean') {
    applyToggleState('ftbAutoDimToggleBtn', enabled);
    setAutoDimOptionsDisabled(!enabled);
  }
  if (typeof dimOpacity === 'number') {
    toolbarAutoDimOpacity = clampNumberStepperValue(dimOpacity, OPACITY_MIN, OPACITY_MAX, 50);
    renderNumberStepper(TOOLBAR_DIM_OPACITY_IDS, toolbarAutoDimOpacity, OPACITY_MIN, OPACITY_MAX);
  }
  if (typeof delay === 'number') {
    autoDimDelay = clampAutoDimDelay(delay);
    renderNumberStepper(AUTO_DIM_DELAY_IDS, autoDimDelay, AUTO_DIM_DELAY_MIN, AUTO_DIM_DELAY_MAX);
  }
}

export function clampAutoHideDelay(value: number): number {
  if (!Number.isFinite(value)) return AUTO_HIDE_DELAY_DEFAULT;
  return Math.min(AUTO_HIDE_DELAY_MAX, Math.max(AUTO_HIDE_DELAY_MIN, Math.round(value)));
}

export function applyFloatingToolbarAutoHideConfig(enabled?: boolean, delay?: number): void {
  if (typeof enabled === 'boolean') {
    applyToggleState('ftbAutoHideToggleBtn', enabled);
    setAutoHideDelayDisabled(!enabled);
  }
  if (typeof delay === 'number') {
    autoHideDelay = clampAutoHideDelay(delay);
    renderNumberStepper(AUTO_HIDE_DELAY_IDS, autoHideDelay, AUTO_HIDE_DELAY_MIN, AUTO_HIDE_DELAY_MAX);
  }
}

export function applyFloatingToolbarShadowConfig(enabled?: boolean): void {
  if (typeof enabled !== 'boolean') return;
  applyToggleState('ftbShadowToggleBtn', enabled);
  setPreviewShadow(enabled);
}

function setPreviewShadow(enabled: boolean): void {
  document.getElementById('ftbPreviewHost')?.classList.toggle('no-shadow', !enabled);
}

// 自动隐藏延时与自动降低不透明度的两个子选项都随各自总开关置灰：行置灰 +
// 控件停收 tab，刷新一次 ± 的禁用态。
function setAutoHideDelayDisabled(disabled: boolean): void {
  document.getElementById('ftbAutoHideDelayRow')?.classList.toggle('is-disabled', disabled);
  setNumberStepperTabIndex(AUTO_HIDE_DELAY_IDS, disabled);
  renderNumberStepper(AUTO_HIDE_DELAY_IDS, autoHideDelay, AUTO_HIDE_DELAY_MIN, AUTO_HIDE_DELAY_MAX);
}

function setAutoDimOptionsDisabled(disabled: boolean): void {
  document.getElementById('ftbAutoDimOpacityRow')?.classList.toggle('is-disabled', disabled);
  document.getElementById('ftbAutoDimDelayRow')?.classList.toggle('is-disabled', disabled);
  setNumberStepperTabIndex(TOOLBAR_DIM_OPACITY_IDS, disabled);
  setNumberStepperTabIndex(AUTO_DIM_DELAY_IDS, disabled);
  renderNumberStepper(AUTO_DIM_DELAY_IDS, autoDimDelay, AUTO_DIM_DELAY_MIN, AUTO_DIM_DELAY_MAX);
}

function normalizeScaleKey(scale: number): string {
  if (Math.abs(scale - 0.75) < 0.001) return '0.75';
  if (Math.abs(scale - 1.25) < 0.001) return '1.25';
  if (Math.abs(scale - 1.5) < 0.001) return '1.5';
  return '1';
}

function mountFloatingToolbarPreview(): void {
  const host = document.getElementById('ftbPreviewHost');
  if (!host) return;

  const source = new DOMParser().parseFromString(ftbHTML, 'text/html');
  const statusBar = source.querySelector<HTMLElement>('.status-bar');
  if (!statusBar) return;

  statusBar.querySelectorAll('#en, #fullwidth, #puncEn').forEach((element) => element.remove());
  statusBar.querySelectorAll<HTMLElement>('[id]').forEach((element) => element.removeAttribute('id'));
  applyToolbarIconGlyphFallbacks(statusBar);
  host.replaceChildren(statusBar);
  updatePreviewItems();
  applyPreviewAppearance();
}

function updatePreviewItems(): void {
  const host = document.getElementById('ftbPreviewHost');
  if (!host) return;
  toolbarItems.forEach((item) => {
    const element = host.querySelector<HTMLElement>(`[data-toolbar-item="${item}"]`);
    if (element) element.style.display = toolbarItemState[item] ? 'flex' : 'none';
  });
}

function applyPreviewAppearance(): void {
  const host = document.getElementById('ftbPreviewHost');
  if (!host) return;
  host.style.setProperty('--ftb-scale', String(toolbarScale));
  host.style.setProperty('--ftb-icon-size', `${toolbarFontSize}px`);
  // 预览直接演示背景不透明度；前景只在 D2D 渲染下区分，预览不做拆分。
  host.style.opacity = String(toolbarBackgroundOpacity / 100);
}
