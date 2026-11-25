import type { Options } from "./options";
export interface Shape { readonly kind: string; area(): number; }
enum Color { Red, Green = 2 }
abstract class Base<T extends object> implements Shape {
  private cache: Map<string, T> = new Map();
  constructor(public readonly kind: string) {}
  abstract area(): number;
}
declare namespace NS { type Id = number | string; }
function first<T>(xs: T[]): T | undefined { return xs[0] as T; }
const n: number = 5, s = "x" satisfies string;
let u: unknown = null;
