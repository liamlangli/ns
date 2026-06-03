// The text buffer / cursor / selection model moved into @liamlangli/ui's
// `code_editor` plugin. This module is kept as a thin re-export so existing
// imports (`import { text_buffer } from './editor.ts'`) keep working.

export { text_buffer } from '@liamlangli/ui';
export type { cursor_pos, selection, selection_range } from '@liamlangli/ui';
