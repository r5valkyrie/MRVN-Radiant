/*
   Fast tokeniser for .map file loading.
   Operates directly on an in-memory buffer instead of going through
   a character-by-character state machine, providing significantly
   faster parsing for large map files.
*/

#pragma once

#include "iscriplib.h"
#include "idatastream.h"
#include <vector>
#include <cstring>
#include <algorithm>

class MapTokeniser final : public Tokeniser
{
	std::vector<char> m_buffer;
	const char* m_cur;
	const char* m_end;
	std::size_t m_line;
	std::size_t m_column;

	char m_token[MAXTOKEN];
	bool m_crossline;
	bool m_unget;

	void skipWhitespaceAndComments(){
		for ( ;; )
		{
			// skip whitespace
			while ( m_cur < m_end && static_cast<unsigned char>( *m_cur ) <= ' ' )
			{
				if ( *m_cur == '\n' ){
					++m_line;
					m_column = 1;
				}
				else{
					++m_column;
				}
				++m_cur;
			}
			if ( m_cur >= m_end ){
				return;
			}

			// check for comments
			if ( *m_cur == '/' && m_cur + 1 < m_end ){
				if ( m_cur[1] == '/' ){
					// line comment — skip to end of line
					m_cur += 2;
					while ( m_cur < m_end && *m_cur != '\n' ){
						++m_cur;
					}
					continue; // re-enter loop to handle the newline
				}
				else if ( m_cur[1] == '*' ){
					// block comment — skip to */
					m_cur += 2;
					m_column += 2;
					while ( m_cur + 1 < m_end ){
						if ( *m_cur == '\n' ){
							++m_line;
							m_column = 1;
						}
						else{
							++m_column;
						}
						if ( *m_cur == '*' && m_cur[1] == '/' ){
							m_cur += 2;
							m_column += 2;
							break;
						}
						++m_cur;
					}
					continue;
				}
			}
			return; // not whitespace or comment
		}
	}

	bool tokenise(){
		if ( !m_crossline ){
			// caller hasn't called nextLine() — check for newline
		}
		skipWhitespaceAndComments();

		if ( m_cur >= m_end ){
			return false;
		}

		char* write = m_token;
		const char* const writeEnd = m_token + MAXTOKEN - 1;

		if ( *m_cur == '"' ){
			// quoted token
			++m_cur;
			++m_column;
			while ( m_cur < m_end && *m_cur != '"' ){
				if ( write < writeEnd ){
					*write++ = *m_cur;
				}
				if ( *m_cur == '\n' ){
					++m_line;
					m_column = 1;
				}
				else{
					++m_column;
				}
				++m_cur;
			}
			if ( m_cur < m_end ){
				++m_cur; // skip closing quote
				++m_column;
			}
		}
		else{
			// regular token — scan until whitespace or quote
			while ( m_cur < m_end && static_cast<unsigned char>( *m_cur ) > ' ' && *m_cur != '"' ){
				if ( write < writeEnd ){
					*write++ = *m_cur;
				}
				++m_cur;
				++m_column;
			}
		}

		*write = '\0';
		return write != m_token;
	}

public:
	MapTokeniser( TextInputStream& stream )
		: m_line( 1 ), m_column( 1 ), m_crossline( false ), m_unget( false ){
		// read entire stream into buffer
		m_buffer.resize( 1 << 20 ); // start with 1MB
		std::size_t total = 0;
		for ( ;; ){
			const std::size_t read = stream.read( m_buffer.data() + total, m_buffer.size() - total );
			total += read;
			if ( read == 0 ){
				break;
			}
			if ( total == m_buffer.size() ){
				m_buffer.resize( m_buffer.size() * 2 );
			}
		}
		m_buffer.resize( total );
		m_cur = m_buffer.data();
		m_end = m_cur + total;
		m_token[0] = '\0';
	}

	void release() override {
		delete this;
	}
	void nextLine() override {
		m_crossline = true;
	}
	const char* getToken() override {
		if ( m_unget ){
			m_unget = false;
			return m_token;
		}
		if ( !tokenise() ){
			return nullptr;
		}
		return m_token;
	}
	void ungetToken() override {
		m_unget = true;
	}
	std::size_t getLine() const override {
		return m_line;
	}
	std::size_t getColumn() const override {
		return m_column;
	}
	bool bufferContains( const char* str ) const override {
		const std::size_t len = std::strlen( str );
		if ( len == 0 ){
			return true;
		}
		// search remaining unread buffer
		const std::size_t remaining = static_cast<std::size_t>( m_end - m_cur );
		if ( remaining < len ){
			return false;
		}
		return std::search( m_cur, m_end, str, str + len ) != m_end;
	}
};

inline Tokeniser& NewMapTokeniser( TextInputStream& istream ){
	return *( new MapTokeniser( istream ) );
}
