/*
 *
 *		Copyright (C) 2020 Michael J. Walsh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */


#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <cairo.h>

#include "utils/RegExp.h"
#include "SubtitleRenderer.h"
#include "DispmanxLayer.h"
#include "Subtitle.h"

using namespace std;

SubtitleRenderer::SubtitleRenderer(int display_num, int layer_num, float r_font_size,
	bool centered, bool box_opacity, unsigned int lines)
: m_centered(centered),
  m_ghost_box(box_opacity),
  m_max_lines(lines)
{
	// Subtitle tag parser regexes
	m_tags = new CRegExp(true);
	m_tags->RegComp("(<[^>]*>|\\{\\\\[^\\}]*\\})");

	m_font_color_html = new CRegExp(true);
	m_font_color_html->RegComp("color[ \\t]*=[ \\t\"']*#?([a-f0-9]{6})");

	m_font_color_curly = new CRegExp(true);
	m_font_color_curly->RegComp("^\\{\\\\c&h([a-f0-9]{2})([a-f0-9]{2})([a-f0-9]{2})&\\}$");

	// Determine screen size
	int screen_width, screen_height;
	DispmanxLayer::openDisplay(display_num, screen_width, screen_height);

	//Calculate font as thousands of screen height
	m_font_size = screen_height * r_font_size;

	// Calculate padding as 1/4 of the font size
	m_padding = m_font_size / 4;

	// And line_height combines the two
	int line_height = m_font_size + m_padding;

	// Calculate image height - must be evenly divisible by 16
	m_image_height = (m_max_lines * line_height) + 5;
	m_image_height = (m_image_height + 15) & ~15; // grow to fit

	m_image_width = screen_width - 100; // avoid screen overshooting
	m_image_width = m_image_width & ~15; // shrink to fit

	// make sure image doesn't overshoot screen
	int left_margin = (screen_width - m_image_width) / 2;

	// bottom margin (relative to top)
	int top_margin = screen_height - m_image_height - (line_height / 2);

	// A fairly unscientific survey showed that with a font size of 59px subtitles lines
	// were rarely longer than 1300px. We also assume that marger font sizes (frequently used
	// in East Asian scripts) would result in shorter not longer subtitles.
	int assumed_longest_subtitle_line_in_pixels = 1300;

	if(screen_width > assumed_longest_subtitle_line_in_pixels)
		left_aligned_margin = (int)(screen_width - assumed_longest_subtitle_line_in_pixels) / 2;
	else if(screen_width > screen_height)
		left_aligned_margin = (int)(screen_width - screen_height) / 2;

	if(left_aligned_margin > left_margin) left_aligned_margin -= left_margin;

	// scaled version for image and screen vars for scaled dvd subtitle layer
	// scale factor assumes a dvd font size of 30 pixels
	float vscale = (screen_height * r_font_size) / 30;
	float hscale = vscale * 1.42;

	m_scaled_image_width = m_image_width / hscale;
	m_scaled_image_height = m_image_height / vscale;
	m_scaled_padding = m_padding / vscale;

	// Create image layers
	subtitleLayer = new DispmanxLayer(layer_num, 4, left_margin, top_margin, m_image_width, m_image_height);
	dvdSubLayer = new DispmanxLayer(layer_num, 1, left_margin, top_margin, m_image_width, m_image_height, m_scaled_image_width, m_scaled_image_height);

	// font faces
	cairo_font_face_t *normal_font = cairo_toy_font_face_create("FreeSans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_font_face_t *italic_font = cairo_toy_font_face_create("FreeSans", CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_font_face_t *bold_font = cairo_toy_font_face_create("FreeSans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

	// prepare scaled fonts
    cairo_matrix_t sizeMatrix, ctm;
    cairo_matrix_init_identity(&ctm);
    cairo_matrix_init_scale(&sizeMatrix, m_font_size, m_font_size);
    cairo_font_options_t *options = cairo_font_options_create();

	m_normal_font_scaled = cairo_scaled_font_create(normal_font, &sizeMatrix, &ctm, options);
	m_italic_font_scaled = cairo_scaled_font_create(italic_font, &sizeMatrix, &ctm, options);
	m_bold_font_scaled = cairo_scaled_font_create(bold_font, &sizeMatrix, &ctm, options);

	// font colours
	m_ghost_box_transparency = cairo_pattern_create_rgba(0, 0, 0, 0.5f);
	m_default_font_color = cairo_pattern_create_rgba(0.866667, 0.866667, 0.866667, 1);
	m_black_font_outline = cairo_pattern_create_rgba(0, 0, 0, 1);

	// cleanup
	cairo_font_options_destroy(options);
	cairo_font_face_destroy(normal_font);
	cairo_font_face_destroy(italic_font);
	cairo_font_face_destroy(bold_font);
}

void SubtitleRenderer::set_font(int new_font_type)
{
	if(new_font_type == m_current_font) return;

	switch(new_font_type) {
		case NORMAL_FONT:
			cairo_set_scaled_font(m_cr, m_normal_font_scaled);
			break;
		case BOLD_FONT:
			cairo_set_scaled_font(m_cr, m_bold_font_scaled);
			break;
		case ITALIC_FONT:
			cairo_set_scaled_font(m_cr, m_italic_font_scaled);
			break;
	}

	m_current_font = new_font_type;
}

void SubtitleRenderer::set_color(int new_color)
{
	if(new_color == m_color) return;

	if(new_color == -1)
		cairo_set_source(m_cr, m_default_font_color);
	else if(new_color == -2)
		cairo_set_source(m_cr, m_ghost_box_transparency);
	else if(new_color == 0)
		cairo_set_source(m_cr, m_black_font_outline);
	else {
		float r = ((new_color >> 16) & 0xFF) / 255.0f;
		float g = ((new_color >>  8) & 0xFF) / 255.0f;
		float b = ((new_color >>  0) & 0xFF) / 255.0f;

		cairo_set_source_rgba(m_cr, r, g, b, 1);
	}

	m_color = new_color;
}


void SubtitleRenderer::prepare(Subtitle &sub)
{
	unprepare();

	if(sub.isImage)
		make_subtitle_image(sub.width, sub.height, sub.image_data);
	else
		parse_lines(sub.text_lines);
}

void SubtitleRenderer::prepare(vector<string> &lines)
{
    unprepare();

    parse_lines(lines);
}

void SubtitleRenderer::make_subtitle_image(vector<vector<SubtitleText> > &parsed_lines)
{
	// create surface
	m_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, m_image_width, m_image_height);
	m_cr = cairo_create(m_surface);

	// Reset font control vars as no font or drawing dolour has been set
	m_current_font = -500;
	m_color = -500;

	// cursor y position
	int cursor_y_position = m_image_height - m_padding;

	// Limit the number of line
	int no_of_lines = parsed_lines.size();
	if(no_of_lines > m_max_lines) no_of_lines = m_max_lines;

	for(int i = no_of_lines - 1; i > -1; i--) {
		int box_width = (m_padding * 2);
		int text_parts = parsed_lines[i].size();

		// cursor x position
		int cursor_x_position = left_aligned_margin;

		for(int j = 0; j < text_parts; j++) {
			set_font(parsed_lines[i][j].font);

			// prepare font glyphs
			cairo_status_t status = cairo_scaled_font_text_to_glyphs(
					cairo_get_scaled_font(m_cr),
					cursor_x_position + m_padding,
					cursor_y_position - (m_padding / 4),
					parsed_lines[i][j].text.c_str(), -1,
					&parsed_lines[i][j].glyphs,
					&parsed_lines[i][j].num_glyphs,
					NULL, NULL, NULL);

			if (status != CAIRO_STATUS_SUCCESS)
				return;

			// calculate font extents
			cairo_text_extents_t extents;
			cairo_glyph_extents (m_cr,
				parsed_lines[i][j].glyphs,
				parsed_lines[i][j].num_glyphs,
				&extents);

			cursor_x_position += extents.x_advance;
			box_width += extents.x_advance;
		}

		// aligned text
		if(m_centered) {
			cursor_x_position = (m_image_width / 2) - (box_width / 2);

			for(int j = 0; j < text_parts; j++) {
				cairo_glyph_t *p = parsed_lines[i][j].glyphs;
				for(int h = 0; h < parsed_lines[i][j].num_glyphs; h++, p++) {
					p->x +=cursor_x_position;
				}
			}
		} else {
			cursor_x_position = left_aligned_margin;
		}

		// draw ghost box
		if(m_ghost_box) {
			set_color(-2);
			cairo_rectangle(m_cr, cursor_x_position, cursor_y_position - m_font_size, box_width,
				m_font_size + m_padding);
			cairo_fill(m_cr);
		}

		for(int j = 0; j < text_parts; j++) {
			set_font(parsed_lines[i][j].font);
			set_color(parsed_lines[i][j].color);

			// draw text
			cairo_glyph_path(m_cr, parsed_lines[i][j].glyphs, parsed_lines[i][j].num_glyphs);

			// free glyph array
			cairo_glyph_free(parsed_lines[i][j].glyphs);
		}

		// draw black text outline
		cairo_fill_preserve(m_cr);
		set_color(0);
		cairo_set_line_width(m_cr, 2);
		cairo_stroke(m_cr);

		// next line
		cursor_y_position -= m_font_size + m_padding;
	}

	cairo_image_data = cairo_image_surface_get_data(m_surface);
	m_prepared_from_text = true;
}


void SubtitleRenderer::make_subtitle_image(int &sub_width, int &sub_height, basic_string<unsigned char> &pixels)
{
	unsigned char *p;

	// Subtitles which exceed dimensions are ignored
	if(sub_width < 1 || sub_width > m_scaled_image_width || sub_height < 1 || sub_height > m_scaled_image_height)
		return;

	p = other_image_data = (unsigned char *)malloc(m_scaled_image_width * m_scaled_image_height);

	auto mem_set = [&p](int num_pixels)
	{
		memset(p, '\0', num_pixels);
		p += num_pixels;
	};

	auto mem_copy = [&p](const unsigned char *pixel, int len)
	{
		memcpy(p, pixel, len);
		p += len;
	};

	// m_scaled_image_width and sub_width can be odd numbers
	int left_padding = (m_scaled_image_width / 2) - (sub_width / 2);
	int right_padding = m_scaled_image_width - sub_width - left_padding;

	int bottom_padding = m_scaled_padding;
	int top_padding = m_scaled_image_height - sub_height - bottom_padding;

	// Subtitles which exceed dimensions are ignored
	if(left_padding < 0 || right_padding < 0 || bottom_padding < 0 || top_padding < 0)
		return;

	// blanks char at top
	mem_set(top_padding * m_scaled_image_width);

	for(int j = 0; j < sub_height; j++) {
		mem_set(left_padding);
		const unsigned char *x = pixels.data() + (j * sub_width);
		mem_copy(x, sub_width);
		mem_set(right_padding);
	}

	// blanks char at bottom
	mem_set(bottom_padding * m_scaled_image_width);

	m_prepared_from_image = true;
}

void SubtitleRenderer::show_next()
{
	if(m_prepared_from_image) {
		subtitleLayer->hideElement();
		dvdSubLayer->setImageData(other_image_data);
		unprepare();
	} else if(m_prepared_from_text) {
		dvdSubLayer->hideElement();
		subtitleLayer->setImageData(cairo_image_data);
		unprepare();
	}
}

void SubtitleRenderer::hide()
{
	subtitleLayer->hideElement();
	dvdSubLayer->hideElement();
}

void SubtitleRenderer::unprepare()
{
	if(m_prepared_from_image) {
		free(other_image_data);
		m_prepared_from_image = false;
	}

	if(m_prepared_from_text) {
		cairo_destroy(m_cr);
		cairo_surface_destroy(m_surface);
		m_prepared_from_text = false;
	}
}

// Tag parser functions
void SubtitleRenderer::parse_lines(vector<string> &text_lines)
{
	vector<vector<SubtitleText> > formatted_lines(text_lines.size());

	bool bold = false, italic = false;
	int color = -1;

	for(uint i=0; i < text_lines.size(); i++) {
		boost::algorithm::trim(text_lines[i]);

		int pos = 0, old_pos = 0;

		int line_length = text_lines[i].length();
		while (pos < line_length) {
			pos = m_tags->RegFind(text_lines[i].c_str(), pos);

			//parse text
			if(pos != old_pos) {
				string t = text_lines[i].substr(old_pos, pos - old_pos);
				int font = italic ? ITALIC_FONT : (bold ? BOLD_FONT : NORMAL_FONT);
				formatted_lines[i].emplace_back(move(t), font, color);
			}

			// No more tags found
			if(pos < 0) break;

			// Parse Tag
			string fullTag = m_tags->GetMatch(0);
			boost::algorithm::to_lower(fullTag);
			pos += fullTag.length();
			old_pos = pos;

			if (fullTag == "<b>" || fullTag == "{\\b1}") {
				bold = true;
			} else if ((fullTag == "</b>" || fullTag == "{\\b0}") && bold) {
				bold = false;
			} else if (fullTag == "<i>" || fullTag == "{\\i1}") {
				italic = true;
			} else if ((fullTag == "</i>" || fullTag == "{\\i0}") && italic) {
				italic = false;
			} else if ((fullTag == "</font>" || fullTag == "{\\c}") && color != -1) {
				color = -1;
			} else if (fullTag.substr(0,5) == "<font") {
				if(m_font_color_html->RegFind(fullTag.c_str(), 5) >= 0) {
					color = hex2int(m_font_color_html->GetMatch(1).c_str());
				}
			} else if(m_font_color_curly->RegFind(fullTag.c_str(), 0) >= 0) {
				string t = m_font_color_curly->GetMatch(3) + m_font_color_curly->GetMatch(2)
					+ m_font_color_curly->GetMatch(1);
				color = hex2int(t.c_str());
			}
		}
	}

	make_subtitle_image(formatted_lines);
}

// expects 6 lowercase, digit hex string
int SubtitleRenderer::hex2int(const char *hex)
{
	int r = 0;
	for(int i = 0, f = 20; i < 6; i++, f -= 4)
		if(hex[i] >= 'a')
			r += (hex[i] - 87) << f;
		else
			r += (hex[i] - 48) << f;

	return r;
}

SubtitleRenderer::~SubtitleRenderer()
{
	//destroy cairo surface, if defined
	unprepare();

	// remove DispmanX layer
	delete subtitleLayer;
	delete dvdSubLayer;
	DispmanxLayer::closeDisplay();

	// destroy cairo fonts
	cairo_scaled_font_destroy(m_normal_font_scaled);
	cairo_scaled_font_destroy(m_italic_font_scaled);
	cairo_scaled_font_destroy(m_bold_font_scaled);

	//delete regexes
	delete m_tags;
	delete m_font_color_html;
	delete m_font_color_curly;
}
