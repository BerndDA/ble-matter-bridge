/*
 * Status JSON for the gateway web page (web_server.h): a small JSON writer and the
 * sections every gateway shares (system, Thread, Matter). An app adds its own sections.
 */

#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace Gateway::Web
{

/* Appends to a fixed buffer; remembers an overflow instead of failing on every call. */
class Json {
public:
	Json(char *buf, size_t size) : mBuf(buf), mSize(size) {}

	void Raw(const char *fmt, ...)
	{
		if (mOverflow) {
			return;
		}
		va_list args;
		va_start(args, fmt);
		const int n = vsnprintf(mBuf + mLength, mSize - mLength, fmt, args);
		va_end(args);
		if (n < 0 || size_t(n) >= mSize - mLength) {
			mOverflow = true;
			return;
		}
		mLength += size_t(n);
	}

	/* "key": with a comma before it unless it opens the object or array. */
	void Key(const char *key)
	{
		Comma();
		Raw("\"%s\":", key);
		mFirst = true;
	}

	void Str(const char *key, const char *value, size_t length = SIZE_MAX)
	{
		if (key) {
			Key(key);
		}
		Comma();
		Raw("\"");
		for (size_t i = 0; i < length && value[i]; i++) {
			const unsigned char c = value[i];
			if (c == '"' || c == '\\') {
				Raw("\\%c", c);
			} else if (c < 0x20) {
				Raw("\\u%04x", c);
			} else {
				Raw("%c", c);
			}
		}
		Raw("\"");
		mFirst = false;
	}

	void Int(const char *key, long long value)
	{
		Key(key);
		Raw("%lld", value);
		mFirst = false;
	}

	void Bool(const char *key, bool value)
	{
		Key(key);
		Raw(value ? "true" : "false");
		mFirst = false;
	}

	void Null(const char *key)
	{
		Key(key);
		Raw("null");
		mFirst = false;
	}

	/* Opens an object or array; @p key is null inside an array. */
	void Open(const char *key, char bracket)
	{
		if (key) {
			Key(key);
		}
		Comma();
		Raw("%c", bracket);
		mFirst = true;
	}

	void Close(char bracket)
	{
		Raw("%c", bracket);
		mFirst = false;
	}

	size_t Done() const { return mOverflow ? 0 : mLength; }

private:
	void Comma()
	{
		if (!mFirst) {
			Raw(",");
		}
		mFirst = true;
	}

	char *mBuf;
	size_t mSize;
	size_t mLength = 0;
	bool mOverflow = false;
	bool mFirst = true;
};

/* "system", "thread" and "matter" objects. Matter thread, without the OpenThread lock. */
void WriteCommonStatus(Json &j);

} /* namespace Gateway::Web */
