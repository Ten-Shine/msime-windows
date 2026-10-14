/// <reference types="node" />
import { expect, it, vi } from 'vitest';
import { clampNumberStepperValue, renderNumberStepper, setNumberStepperTabIndex, setupNumberStepper } from './number-stepper';

const IDS = { dec: 'decBtn', input: 'valInput', inc: 'incBtn' };

it('clamps stepper values into the integer range', () => {
  expect(clampNumberStepperValue(0, 1, 100, 50)).toBe(1);
  expect(clampNumberStepperValue(101, 1, 100, 50)).toBe(100);
  expect(clampNumberStepperValue(42, 1, 100, 50)).toBe(42);
  expect(clampNumberStepperValue(2.6, 1, 100, 50)).toBe(3);
  // 解析不了的值弹回 fallback，而不是最小值。
  expect(clampNumberStepperValue(Number.NaN, 1, 100, 50)).toBe(50);
  expect(clampNumberStepperValue(Number.POSITIVE_INFINITY, 1, 100, 50)).toBe(50);
});

it('renders the value and disables the buttons at the bounds', () => {
  const elements = new Map(
    [IDS.dec, IDS.input, IDS.inc].map((id) => [id, { id, value: '', disabled: false, tabIndex: 0 }])
  );
  vi.stubGlobal('document', { getElementById: (id: string) => elements.get(id) ?? null });
  try {
    renderNumberStepper(IDS, 1, 1, 100);
    expect(elements.get(IDS.input)!.value).toBe('1');
    expect(elements.get(IDS.dec)!.disabled).toBe(true);
    expect(elements.get(IDS.inc)!.disabled).toBe(false);

    renderNumberStepper(IDS, 100, 1, 100);
    expect(elements.get(IDS.dec)!.disabled).toBe(false);
    expect(elements.get(IDS.inc)!.disabled).toBe(true);

    setNumberStepperTabIndex(IDS, true);
    expect(elements.get(IDS.input)!.tabIndex).toBe(-1);
    setNumberStepperTabIndex(IDS, false);
    expect(elements.get(IDS.inc)!.tabIndex).toBe(0);
  } finally {
    vi.unstubAllGlobals();
  }
});

it('steps with the buttons, the keyboard and typed values, committing only real changes', () => {
  const listeners = new Map<string, (event?: unknown) => void>();
  const element = (id: string) => ({
    id,
    value: '',
    disabled: false,
    tabIndex: 0,
    addEventListener: (type: string, listener: (event?: unknown) => void) => {
      if (type === 'click' || type === 'change' || type === 'keydown') listeners.set(`${id}:${type}`, listener);
    }
  });
  const elements = new Map([IDS.dec, IDS.input, IDS.inc].map((id) => [id, element(id)]));
  vi.stubGlobal('document', { getElementById: (id: string) => elements.get(id) ?? null });
  const onCommit = vi.fn();
  let current = 50;
  try {
    setupNumberStepper(IDS, {
      min: 1,
      max: 100,
      fallback: 50,
      getValue: () => current,
      onCommit: (value) => {
        current = value;
        onCommit(value);
      }
    });

    listeners.get(`${IDS.inc}:click`)!();
    expect(onCommit).toHaveBeenLastCalledWith(51);
    listeners.get(`${IDS.dec}:click`)!();
    expect(onCommit).toHaveBeenLastCalledWith(50);

    // 到下限再按 −：值不变，不上报。
    current = 1;
    onCommit.mockClear();
    listeners.get(`${IDS.dec}:click`)!();
    expect(onCommit).not.toHaveBeenCalled();
    expect(elements.get(IDS.input)!.value).toBe('1');
    expect(elements.get(IDS.dec)!.disabled).toBe(true);

    // 方向键与回车走同一条提交；解析不了的输入弹回当前值。
    current = 60;
    listeners.get(`${IDS.input}:keydown`)!({ key: 'ArrowUp', preventDefault: vi.fn() });
    expect(onCommit).toHaveBeenLastCalledWith(61);
    elements.get(IDS.input)!.value = 'not-a-number';
    listeners.get(`${IDS.input}:change`)!();
    expect(onCommit).toHaveBeenLastCalledWith(61);
    expect(elements.get(IDS.input)!.value).toBe('61');

    elements.get(IDS.input)!.value = '73';
    listeners.get(`${IDS.input}:keydown`)!({ key: 'Enter', preventDefault: vi.fn() });
    expect(onCommit).toHaveBeenLastCalledWith(73);
  } finally {
    vi.unstubAllGlobals();
  }
});
