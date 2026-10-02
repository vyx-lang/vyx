// Escape before adding markup; examples stay readable as their original source.
const escape = text => text.replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;')
const tokens = /\/\/[^\n]*|"(?:[^"\\]|\\.)*"|(?:@|#)!?\[[a-zA-Z_]+|#pragma\b|\b(?:fn|let|var|return|use|extern|struct|class|module|public|private|mut|if|else|true|false|self|pub|impl|where|template|typename|const|noexcept|virtual|override|unsafe|trait|dyn|as|explicit|type)\b|\b(?:i32|i64|u64|f64|bool|void|double|int|T|U|A|B)\b|\b\d+(?:\.\d+)?\b/g

export function highlightSource(source) {
  let html = '', end = 0
  for (const match of source.matchAll(tokens)) {
    html += escape(source.slice(end, match.index))
    const value = match[0]
    const kind = value.startsWith('//') ? 'comment' : value.startsWith('"') ? 'string'
      : /^[#@]/.test(value) ? 'attribute' : /^\d/.test(value) ? 'number'
        : /^(i32|i64|u64|f64|bool|void|double|int|T|U|A|B)$/.test(value) ? 'type' : 'keyword'
    html += `<span class="token-${kind}">${escape(value)}</span>`
    end = match.index + value.length
  }
  return html + escape(source.slice(end))
}
