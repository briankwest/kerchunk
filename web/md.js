/* md.js — tiny subset-markdown renderer for the repeater bulletin.
 *
 * Covers the minimum useful for announcements: headings (#..######),
 * paragraphs, unordered lists, blockquotes, bold/italic/code inlines,
 * [text](url) links and bare http(s) auto-links.
 *
 * Safety: HTML-escapes the input first, then reinserts only the small
 * set of tags we generate. User-typed <script>/<img>/<iframe> etc. show
 * as literal text. Link hrefs are restricted to http(s), relative ('/',
 * '#') and mailto:.
 */
window.renderMarkdown = (function() {

  function escapeHtml(s) {
    return s.replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;')
            .replace(/'/g, '&#39;');
  }

  function safeUrl(u) {
    return /^(https?:\/\/|\/|#|mailto:)/i.test(u) ? u : '';
  }

  function renderInline(s) {
    /* [text](url) — explicit links */
    s = s.replace(/\[([^\]\n]+)\]\(([^)\s]+)\)/g, function(_, text, url) {
      var u = safeUrl(url);
      if (!u) return text;
      return '<a href="' + u + '" target="_blank" rel="noopener noreferrer">' + text + '</a>';
    });
    /* Bare http(s) auto-links (skip ones already inside an <a ...>) */
    s = s.replace(/(^|[^"\/>])\b(https?:\/\/[^\s<]+)/g, function(_, pre, url) {
      return pre + '<a href="' + url + '" target="_blank" rel="noopener noreferrer">' + url + '</a>';
    });
    s = s.replace(/`([^`\n]+)`/g, '<code>$1</code>');
    s = s.replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>');
    s = s.replace(/\*([^*\n]+)\*/g, '<em>$1</em>');
    return s;
  }

  return function renderMarkdown(src) {
    if (!src) return '';
    var esc = escapeHtml(src);
    var blocks = esc.split(/\n{2,}/);
    var out = [];
    for (var i = 0; i < blocks.length; i++) {
      var raw = blocks[i];
      if (!raw.trim()) continue;
      var lines = raw.split('\n');
      var first = lines[0];
      var m = first.match(/^(#{1,6})\s+(.+)$/);
      if (m) {
        var level = m[1].length;
        out.push('<h' + level + '>' + renderInline(m[2]) + '</h' + level + '>');
        continue;
      }
      var isList = lines.every(function(l) { return /^\s*[-*]\s+/.test(l); });
      if (isList) {
        out.push('<ul>' + lines.map(function(l) {
          return '<li>' + renderInline(l.replace(/^\s*[-*]\s+/, '')) + '</li>';
        }).join('') + '</ul>');
        continue;
      }
      var isQuote = lines.every(function(l) { return /^>\s?/.test(l); });
      if (isQuote) {
        var body = lines.map(function(l) { return l.replace(/^>\s?/, ''); }).join('<br>');
        out.push('<blockquote>' + renderInline(body) + '</blockquote>');
        continue;
      }
      out.push('<p>' + renderInline(lines.join('<br>')) + '</p>');
    }
    return out.join('\n');
  };
})();
