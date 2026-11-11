#!/usr/bin/env python3
"""Builds the players' landing page from README.md, so the page always says what the README says.

  python3 docs/landing/build_page.py <output folder>

Needs the Python package "markdown". The page is the README without GitHub's file table: same sections, same
words. Links to files of the repository go to GitHub; pictures are copied next to the page.
"""
import os, re, shutil, sys
import markdown

REPO = 'https://github.com/K11MCH1/Endless-Keeper'
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ALERTS = {'NOTE': 'Note', 'TIP': 'Tip', 'IMPORTANT': 'Important', 'WARNING': 'Warning', 'CAUTION': 'Caution'}


def slug(value, separator='-'):
    """Heading ids: letters, digits and dashes only (the README's own links start with a dash for the emoji)."""
    s = re.sub(r'[^a-z0-9 -]', '', value.lower()).strip()
    return re.sub(r'[ -]+', separator, s)


def build(out):
    src = open(os.path.join(ROOT, 'README.md'), encoding='utf-8').read().replace('\r\n', '\n')
    # markdown inside the README's HTML blocks
    src = re.sub(r'<div align="center">', '<div class="center" markdown="1">', src)
    src = src.replace('<details>', '<details markdown="1">')
    body = markdown.markdown(src, extensions=['tables', 'md_in_html', 'fenced_code', 'sane_lists', 'toc'],
                             extension_configs={'toc': {'slugify': slug}})
    # GitHub's note boxes
    def alert(m):
        kind = m.group(1)
        return '<blockquote class="alert %s"><p><span class="alert-title">%s</span>' % (kind.lower(), ALERTS[kind])
    body = re.sub(r'<blockquote>\s*<p>\[!(%s)\]\s*(?:<br\s*/?>)?\s*' % '|'.join(ALERTS), alert, body)
    # pictures beside the page; other repository files on GitHub
    pictures = set()
    def fix_src(m):
        path = m.group(1)
        if path.startswith(('http://', 'https://', 'data:')):
            return m.group(0)
        pictures.add(path)
        return 'src="images/%s"' % os.path.basename(path)
    body = re.sub(r'src="([^"]+)"', fix_src, body)
    def fix_href(m):
        url = m.group(1)
        if url.startswith(('http://', 'https://', 'mailto:')):
            return m.group(0)
        if url.startswith('#'):
            return 'href="#%s"' % url.lstrip('#-')
        return 'href="%s/blob/main/%s"' % (REPO, url)
    body = re.sub(r'href="([^"]+)"', fix_href, body)
    # wide tables scroll inside their own box on small screens
    body = body.replace('<table>', '<div class="table"><table>').replace('</table>', '</table></div>')

    os.makedirs(os.path.join(out, 'images'), exist_ok=True)
    for p in sorted(pictures):
        shutil.copy(os.path.join(ROOT, p), os.path.join(out, 'images', os.path.basename(p)))
    page = open(os.path.join(HERE, 'template.html'), encoding='utf-8').read().replace('<!--README-->', body)
    open(os.path.join(out, 'index.html'), 'w', encoding='utf-8').write(page)
    open(os.path.join(out, '.nojekyll'), 'w').close()
    print('page: %d bytes, %d pictures, %d headings' % (len(page), len(pictures), len(re.findall(r'<h[1-3]', body))))


if __name__ == '__main__':
    build(sys.argv[1] if len(sys.argv) > 1 else 'site')
