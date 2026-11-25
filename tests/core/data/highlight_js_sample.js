import { readFile as read } from "node:fs";
export default class Counter extends Base {
  static #count = 0;
  get value() { return this._value; }
  async load(path = './a.txt') {
    const MAX = 3, text = await read(path);
    for (const line of text.split(/\r?\n/)) {
      if (line === undefined || typeof line !== 'string') continue;
      console.log(`line ${line.length}: café 😀`, MAX, "tab\t");
    }
    return new Map([[1, true], [2, null]]);
  }
}
/* block
   comment */
const add = (a, b) => a + b ** 2; // sum
function Widget({ title, items }) { var self = this; return items.map(x => x * 0x1F); }
