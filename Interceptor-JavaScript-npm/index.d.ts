export type Kind = 'keyboard' | 'mouse' | 'hid' | 'xinput' | 'xinputhid' | 'gip';
export interface Field { value: number | bigint; normalized: number; pressed: boolean; readonly minimum: number | bigint; readonly maximum: number | bigint; readonly physical_minimum: number | bigint; readonly physical_maximum: number | bigint; readonly normalized_valid: boolean; readonly usage_page: number; readonly usage: number; readonly collection: number; readonly report_id: number; readonly data_index: number; readonly element: number; readonly flags: number; readonly bit_size: number; readonly units: number; readonly units_exponent: number; data: Buffer; }
interface InputBase { readonly token: string; readonly sequence: bigint; readonly timestamp: bigint; blocked: boolean; report: Buffer; readonly decode_error: number; readonly decode_complete: boolean; readonly parser_status: number; }
export interface KeyboardInput extends InputBase { readonly kind: 'keyboard'; state: { scan_code: number; flags: number; information: number }; }
export interface MouseInput extends InputBase { readonly kind: 'mouse'; state: { x: number; y: number; flags: number; buttons: number; wheel: number; raw_buttons: number; information: number }; }
export interface HidInput extends InputBase { readonly kind: 'hid' | 'xinputhid'; state: { axes?: Field[]; triggers?: Field[]; hats?: Field[]; values?: Field[]; buttons?: Field[]; binary_values?: Field[] }; }
export interface GamepadInput extends InputBase { readonly kind: 'xinput' | 'gip'; state: { readonly packet: number; buttons: number; left_trigger: number; right_trigger: number; left_x: number; left_y: number; right_x: number; right_y: number; extra_buttons: number }; }
export type Input = KeyboardInput | MouseInput | HidInput | GamepadInput;
export interface Device { readonly kind: Kind; readonly id: number; readonly token: string; readonly name: string; readonly hardware_id: string; readonly vid: number; readonly pid: number; readonly usage_page: number; readonly usage: number; }
export type Batch = readonly Input[];
export class Interceptor {
  initialize(): {ready: boolean; reboot_required: boolean; associated: number; already_associated: number; skipped: number};
  info(): Record<string, number>;
  devices(): Device[];
  receive(timeout?: number | null): Promise<Batch>;
  send(inputs: Batch): number;
  discard(inputs: Batch): void;
  inject(input: {kind: Kind; token?: string; state?: Record<string, number | bigint>; report?: Buffer}): number;
  descriptor(token: string, report?: boolean): {applicable: boolean; format: number; data: Buffer};
  cancel(): void;
  close(): void;
}
