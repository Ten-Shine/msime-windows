// 数值步进器：− [值] + 三件套，与「自动隐藏延时」同款。值域钳制在
// [min, max] 的整数；−/+、回车、失焦、上下方向键都走同一次提交，解析不了的
// 值弹回当前值。状态归调用方（模块级 let），这里只负责接线、钳制与渲染。
export type NumberStepperIds = {
  dec: string;
  input: string;
  inc: string;
};

export type NumberStepperOptions = {
  min: number;
  max: number;
  /** 解析不了的值（NaN / 空 / 非数字）弹回到它。 */
  fallback: number;
  /** 当前值；提交前与它比较，决定要不要上报。 */
  getValue: () => number;
  /** 值确实变化时调用（已经过钳制）。调用方负责落状态、刷预览、发配置。 */
  onCommit: (value: number) => void;
};

export function clampNumberStepperValue(value: number, min: number, max: number, fallback: number): number {
  if (!Number.isFinite(value)) return fallback;
  return Math.min(max, Math.max(min, Math.round(value)));
}

export function renderNumberStepper(ids: NumberStepperIds, value: number, min: number, max: number): void {
  const input = document.getElementById(ids.input) as HTMLInputElement | null;
  if (input) input.value = String(value);
  const decrement = document.getElementById(ids.dec) as HTMLButtonElement | null;
  const increment = document.getElementById(ids.inc) as HTMLButtonElement | null;
  if (decrement) decrement.disabled = value <= min;
  if (increment) increment.disabled = value >= max;
}

// 行级置灰时同步停收 Tab：与 setAutoHideDelayDisabled 对步进器的处理一致。
export function setNumberStepperTabIndex(ids: NumberStepperIds, disabled: boolean): void {
  [ids.dec, ids.input, ids.inc].forEach((id) => {
    const control = document.getElementById(id);
    if (control) control.tabIndex = disabled ? -1 : 0;
  });
}

export function setupNumberStepper(ids: NumberStepperIds, options: NumberStepperOptions): void {
  const commit = (raw: number): void => {
    const next = clampNumberStepperValue(raw, options.min, options.max, options.fallback);
    const changed = next !== options.getValue();
    renderNumberStepper(ids, next, options.min, options.max);
    if (changed) options.onCommit(next);
  };
  document.getElementById(ids.dec)?.addEventListener('click', () => {
    commit(options.getValue() - 1);
  });
  document.getElementById(ids.inc)?.addEventListener('click', () => {
    commit(options.getValue() + 1);
  });
  const input = document.getElementById(ids.input) as HTMLInputElement | null;
  if (!input) return;
  // Typed values commit on Enter or blur; anything unparsable snaps back.
  const commitTyped = (): void => {
    const text = input.value.trim();
    commit(/^\d+$/.test(text) ? Number(text) : options.getValue());
  };
  input.addEventListener('change', commitTyped);
  input.addEventListener('keydown', (event: KeyboardEvent) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      commitTyped();
    } else if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
      event.preventDefault();
      commit(options.getValue() + (event.key === 'ArrowUp' ? 1 : -1));
    }
  });
}
