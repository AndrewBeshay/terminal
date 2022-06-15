// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "Row.hpp"
#include "textBuffer.hpp"

#pragma warning(push, 1)

// Routine Description:
// - constructor
// Arguments:
// - rowWidth - the width of the row, cell elements
// - fillAttribute - the default text attribute
// Return Value:
// - constructed object
ROW::ROW(wchar_t* buffer, uint16_t* indices, const uint16_t rowWidth, const TextAttribute& fillAttribute) :
    _charsBuffer{ buffer },
    _chars{ buffer },
    _indices{ indices },
    _charsCapacity{ rowWidth },
    _indicesCount{ rowWidth },
    _attr{ rowWidth, fillAttribute }
{
    _init();
}

ROW::~ROW()
{
    _dealloc();
}

void ROW::_dealloc() const noexcept
{
    if (_chars != _charsBuffer)
    {
        delete[] _chars;
    }
    if (_dbcsPaddedColumns)
    {
        delete[] _dbcsPaddedColumns;
    }
}

void ROW::_init() const noexcept
{
    if (_chars)
    {
        std::fill_n(_chars, _indicesCount, UNICODE_SPACE);
        std::iota(_indices, _indices + _indicesCount + 1, static_cast<uint16_t>(0));
    }
}

// Routine Description:
// - Sets all properties of the ROW to default values
// Arguments:
// - Attr - The default attribute (color) to fill
// Return Value:
// - <none>
bool ROW::Reset(const TextAttribute& Attr)
{
    _dealloc();

    _chars = _charsBuffer;
    _dbcsPaddedColumns = nullptr;

    _charsCapacity = _indicesCount;

    _attr = { gsl::narrow_cast<uint16_t>(_indicesCount), Attr };

    _lineRendition = LineRendition::SingleWidth;
    _wrapForced = false;
    _doubleBytePadded = false;

    _init();
    return true;
}

// Routine Description:
// - resizes ROW to new width
// Arguments:
// - width - the new width, in cells
// Return Value:
// - S_OK if successful, otherwise relevant error
void ROW::Resize(wchar_t* const charsBuffer, uint16_t* const indices, const uint16_t newWidth)
{
    uint16_t colsToCopy = 0;
    uint16_t charsToCopy = 0;
    if (_indices)
    {
        colsToCopy = gsl::narrow_cast<uint16_t>(std::min(_indicesCount, newWidth));
        charsToCopy = _indices[colsToCopy];
        for (; colsToCopy != 0 && _indices[colsToCopy - 1] == charsToCopy; --colsToCopy)
        {
        }
    }

    const uint16_t trailingWhitespace = newWidth - colsToCopy;
    const uint16_t charsCapacity = charsToCopy + trailingWhitespace;
    auto chars = charsBuffer;
    if (charsCapacity > newWidth)
    {
        chars = new wchar_t[charsCapacity];
    }

    bool* dbcsPaddedColumns = nullptr;
    if (_dbcsPaddedColumns)
    {
        dbcsPaddedColumns = new bool[newWidth]{};
        std::copy_n(_dbcsPaddedColumns, colsToCopy, dbcsPaddedColumns);
    }

    {
        const auto it = std::copy_n(_chars, charsToCopy, chars);
        std::fill_n(it, trailingWhitespace, L' ');
    }
    {
        const auto it = std::copy_n(_indices, colsToCopy, indices);
        // The _indices array is 1 wider than newWidth indicates.
        // This is because the extra column contains the past-the-end index into _chars.
        std::iota(it, it + trailingWhitespace + 1, charsToCopy);
    }

    _dealloc();

    _charsBuffer = charsBuffer;
    _chars = chars;
    _indices = indices;
    _dbcsPaddedColumns = dbcsPaddedColumns;

    _charsCapacity = charsCapacity;
    _indicesCount = newWidth;

    _attr.resize_trailing_extent(gsl::narrow_cast<uint16_t>(newWidth));
}

void ROW::TransferAttributes(const til::small_rle<TextAttribute, uint16_t, 1>& attr, til::CoordType newWidth)
{
    _attr = attr;
    _attr.resize_trailing_extent(gsl::narrow<uint16_t>(newWidth));
}

// Routine Description:
// - clears char data in column in row
// Arguments:
// - column - 0-indexed column index
// Return Value:
// - <none>
void ROW::ClearCell(const til::CoordType column)
{
    static constexpr std::wstring_view space{ L" " };
    ReplaceCharacters(column, 1, space);
}

// Routine Description:
// - writes cell data to the row
// Arguments:
// - it - custom console iterator to use for seeking input data. bool() false when it becomes invalid while seeking.
// - index - column in row to start writing at
// - wrap - change the wrap flag if we hit the end of the row while writing and there's still more data in the iterator.
// - limitRight - right inclusive column ID for the last write in this row. (optional, will just write to the end of row if nullopt)
// Return Value:
// - iterator to first cell that was not written to this row.
OutputCellIterator ROW::WriteCells(OutputCellIterator it, const til::CoordType index, const std::optional<bool> wrap, std::optional<til::CoordType> limitRight)
{
    THROW_HR_IF(E_INVALIDARG, index >= size());
    THROW_HR_IF(E_INVALIDARG, limitRight.value_or(0) >= size());

    // If we're given a right-side column limit, use it. Otherwise, the write limit is the final column index available in the char row.
    const auto finalColumnInRow = limitRight.value_or(size() - 1);

    auto currentColor = it->TextAttr();
    uint16_t colorUses = 0;
    auto colorStarts = gsl::narrow_cast<uint16_t>(index);
    auto currentIndex = colorStarts;

    while (it && currentIndex <= finalColumnInRow)
    {
        // Fill the color if the behavior isn't set to keeping the current color.
        if (it->TextAttrBehavior() != TextAttributeBehavior::Current)
        {
            // If the color of this cell is the same as the run we're currently on,
            // just increment the counter.
            if (currentColor == it->TextAttr())
            {
                ++colorUses;
            }
            else
            {
                // Otherwise, commit this color into the run and save off the new one.
                // Now commit the new color runs into the attr row.
                _replaceAttributes(colorStarts, currentIndex, currentColor);
                currentColor = it->TextAttr();
                colorUses = 1;
                colorStarts = currentIndex;
            }
        }

        // Fill the text if the behavior isn't set to saying there's only a color stored in this iterator.
        if (it->TextAttrBehavior() != TextAttributeBehavior::StoredOnly)
        {
            const auto fillingLastColumn = currentIndex == finalColumnInRow;
            const auto attr = it->DbcsAttr();
            const auto& chars = it->Chars();

            if (attr.IsSingle())
            {
                ReplaceCharacters(currentIndex, 1, chars);
                ++it;
            }
            else if (attr.IsLeading())
            {
                if (fillingLastColumn)
                {
                    // If we're trying to fill the last cell with a leading byte, pad it out instead by clearing it.
                    // Don't increment iterator. We'll exit because we couldn't write a lead at the end of a line.
                    ClearCell(currentIndex);
                    SetDoubleBytePadded(true);
                }
                else
                {
                    ReplaceCharacters(currentIndex, 2, chars);
                    ++it;
                }
            }
            else
            {
                ++it;
            }

            // If we're asked to (un)set the wrap status and we just filled the last column with some text...
            // NOTE:
            //  - wrap = std::nullopt    --> don't change the wrap value
            //  - wrap = true            --> we're filling cells as a steam, consider this a wrap
            //  - wrap = false           --> we're filling cells as a block, unwrap
            if (wrap.has_value() && fillingLastColumn)
            {
                // set wrap status on the row to parameter's value.
                SetWrapForced(*wrap);
            }
        }
        else
        {
            ++it;
        }

        // Move to the next cell for the next time through the loop.
        ++currentIndex;
    }

    // Now commit the final color into the attr row
    if (colorUses)
    {
        _replaceAttributes(colorStarts, currentIndex, currentColor);
    }

    return it;
}

void ROW::_replaceAttributes(const til::CoordType beginIndex, const til::CoordType endIndex, const TextAttribute& newAttr)
{
    _attr.replace(gsl::narrow<uint16_t>(beginIndex), gsl::narrow<uint16_t>(endIndex), newAttr);
}

bool ROW::SetAttrToEnd(const til::CoordType beginIndex, const TextAttribute attr)
{
    _attr.replace(gsl::narrow<uint16_t>(beginIndex), _attr.size(), attr);
    return true;
}

void ROW::ReplaceCharacters(til::CoordType x, til::CoordType width, const std::wstring_view& chars)
{
    const auto col1 = gsl::narrow<uint16_t>(x);
    const auto col2 = gsl::narrow<uint16_t>(x + width);

    if ((col1 >= col2) | (col2 > _indicesCount) | chars.empty())
    {
        return;
    }

    uint16_t col0 = col1;
    const uint16_t ch0 = _indices[col0];
    for (; col0 != 0 && _indices[col0 - 1] == ch0; --col0)
    {
    }

    uint16_t col3 = col2 - 1;
    uint16_t ch1;
    {
        const uint16_t ch1ref = _indices[col3];
        while ((ch1 = _indices[++col3]) == ch1ref)
        {
        }
    }

    const size_t leadingSpaces = col1 - col0;
    const size_t trailingSpaces = col3 - col2;
    const size_t insertedChars = chars.size() + leadingSpaces + trailingSpaces;
    const size_t newCh1 = insertedChars + ch0;

    if (newCh1 != ch1)
    {
        _resizeChars(ch0, ch1, newCh1, col3);
    }

    {
        auto ch = _chars + ch0;
        auto in0 = _indices + col0;
        const auto in1 = _indices + col1;
        auto in2 = _indices + col2;
        const auto in3 = _indices + col3;
        auto chPos = ch0;

        for (; in0 != in1; ++ch, ++in0, ++chPos)
        {
            *ch = L' ';
            *in0 = chPos;
        }

        ch = std::copy_n(chars.data(), chars.size(), ch);
        std::fill(in1, in2, chPos);
        chPos += chars.size();

        for (; in2 != in3; ++ch, ++in2, ++chPos)
        {
            *ch = L' ';
            *in2 = chPos;
        }
    }
}

void ROW::_resizeChars(uint16_t ch0, uint16_t ch1, size_t newCh1, uint16_t col3)
{
    const auto diff = newCh1 - ch1;
    const auto currentLength = _indices[_indicesCount];
    const auto newLength = _indices[_indicesCount] + diff;

    if (newLength <= _charsCapacity)
    {
        std::copy_n(_chars + ch1, currentLength - ch1, _chars + newCh1);
    }
    else
    {
        const auto minCapacity = static_cast<size_t>(_charsCapacity) + (_charsCapacity >> 1);
        const auto newCapacity = gsl::narrow<uint16_t>(std::max(newLength, minCapacity));
        const auto chars = new wchar_t[newCapacity];

        std::copy_n(_chars, ch0, chars);
        std::copy_n(_chars + ch1, currentLength - ch1, chars + newCh1);

        if (_chars != _charsBuffer)
        {
            delete[] _chars;
        }

        _chars = chars;
        _charsCapacity = newCapacity;
    }

    for (auto it = &_indices[col3], end = &_indices[_indicesCount + 1]; it != end; ++it)
    {
        *it += diff;
    }
}

const til::small_rle<TextAttribute, uint16_t, 1>& ROW::Attributes() const noexcept
{
    return _attr;
}

TextAttribute ROW::GetAttrByColumn(const til::CoordType column) const
{
    return _attr.at(gsl::narrow<uint16_t>(column));
}

std::vector<uint16_t> ROW::GetHyperlinks() const
{
    std::vector<uint16_t> ids;
    for (const auto& run : _attr.runs())
    {
        if (run.value.IsHyperlink())
        {
            ids.emplace_back(run.value.GetHyperlinkId());
        }
    }
    return ids;
}

uint16_t ROW::size() const noexcept
{
    return _indicesCount;
}

til::CoordType ROW::MeasureLeft() const noexcept
{
    const auto beg = _chars;
    const auto end = beg + _indices[_indicesCount];
    auto it = beg;

    for (; it != end; ++it)
    {
        if (*it != L' ')
        {
            break;
        }
    }

    return static_cast<til::CoordType>(it - beg);
}

til::CoordType ROW::MeasureRight() const noexcept
{
    const auto beg = _chars;
    const auto end = beg + _indices[_indicesCount];
    // We can always subtract 1, because _indicesCount/_charsCount are always greater 0.
    auto it = end;

    for (; it != beg; --it)
    {
        if (it[-1] != L' ')
        {
            break;
        }
    }

    return static_cast<til::CoordType>(it - beg);
}

bool ROW::ContainsText() const noexcept
{
    auto it = _chars;
    const auto end = it + _indices[_indicesCount];

    for (; it != end; ++it)
    {
        if (*it != L' ')
        {
            return true;
        }
    }

    return false;
}

InassignableStringView ROW::GlyphAt(til::CoordType column) const noexcept
{
    column = std::min(column, _indicesCount - 1);

    const auto current = _indices[column];
    while (column <= _indicesCount && _indices[++column] == current)
    {
    }

    const auto len = gsl::narrow_cast<size_t>(_indices[column] - current);
    return { _chars + current, len };
}

InassignableDbcsAttribute ROW::DbcsAttrAt(til::CoordType column) const noexcept
{
    column = std::min(column, _indicesCount - 1);

    const auto idx = _indices[column];

    auto attr = DbcsAttribute::Attribute::Single;
    if (column > 0 && _indices[column - 1] == idx)
    {
        attr = DbcsAttribute::Attribute::Trailing;
    }
    else if (column < _indicesCount && _indices[column + 1] == idx)
    {
        attr = DbcsAttribute::Attribute::Leading;
    }

    return { attr };
}

InassignableStringView ROW::GetText() const noexcept
{
    return { _chars, _indices[_indicesCount] };
}

DelimiterClass ROW::DelimiterClassAt(til::CoordType column, const std::wstring_view& wordDelimiters) const noexcept
{
    column = std::min(column, _indicesCount - 1);

    const auto glyph = _chars[_indices[column]];

    if (glyph <= L' ')
    {
        return DelimiterClass::ControlChar;
    }
    else if (wordDelimiters.find(glyph) != std::wstring_view::npos)
    {
        return DelimiterClass::DelimiterChar;
    }
    else
    {
        return DelimiterClass::RegularChar;
    }
}

RowTextIterator ROW::CharsBegin() const noexcept
{
    return { _chars, _indices, _indicesCount, 0, 0 };
}

RowTextIterator ROW::CharsEnd() const noexcept
{
    return { _chars, _indices, _indicesCount, _indicesCount, _indicesCount };
}

bool* ROW::_getDbcsPaddedColumns() noexcept
{
    if (!_dbcsPaddedColumns)
    {
        _dbcsPaddedColumns = new bool[_indicesCount]{};
    }
    return _dbcsPaddedColumns;
}

#pragma warning(pop)
