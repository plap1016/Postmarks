#if defined (_MSC_VER) && (_MSC_VER >= 1000)
#pragma once
#endif
#ifndef NMRH_NUMERIC_RANGE_HANDLER_H
#define NMRH_NUMERIC_RANGE_HANDLER_H

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <set>
#include <limits>
#include <istream>
#include <ostream>

#undef min
#undef max

namespace Nmrh
{

template <class N> class NumericRangeHandler
{
public:
	class NumericRange
	{
	public:
		N m_from;
		N m_to;

		NumericRange(void) : m_from(std::numeric_limits<N>::min()), m_to(std::numeric_limits<N>::max()){}
		NumericRange(N from, N to) : m_from(from), m_to(to){}

		bool operator <(const NumericRange& o) const
		{
			return m_from < o.m_from || (m_from == o.m_from && m_to < o.m_to);
		}

		bool operator ==(const NumericRange& o) const
		{
			return m_from == o.m_from && m_to == o.m_to;
		}

		bool operator !=(const NumericRange& o) const
		{
			return m_from != o.m_from || m_to != o.m_to;
		}

		const NumericRange intersect(const NumericRange& r) const
		{
			NumericRange ret(*this);

			if (r.m_from > ret.m_from /*&& r.m_from < ret.m_to*/)
				ret.m_from = r.m_from;
			if (r.m_to < ret.m_to /*&& r.m_to > ret.m_from*/)
				ret.m_to = r.m_to;

			return ret;
		}

		bool invalid() const { return m_from > m_to; }

		NumericRange& operator += (const NumericRange& r)
		{
			if (r.m_from < m_from && r.m_to >= m_from)
				m_from = r.m_from;
			if (r.m_to > m_to && r.m_from <= m_to)
				m_to = r.m_to;

			return *this;
		}

		template <class S> friend S& operator << (S& o, const NumericRange& r)
		{
			o << r.m_from << ' ' << r.m_to << ';';
			return o;
		}

		template <class S> friend S& operator >> (S& i, NumericRange &r)
		{
			i >> r.m_from >> r.m_to;
			i.seekg(1);
			return i;
		}
	};

	static const NumericRange INVALID_RANGE(void)
	{
		return NumericRange(1, 0);
	};

	static const N MAX_N; //= std::numeric_limits<N>::max();
	static const N MIN_N; //= std::numeric_limits<N>::min();

	class NumericRangeList
	{
		friend class Nmrh::NumericRangeHandler<N>;
		typedef std::set<NumericRange> NRSet;
		NRSet m_s;
	public:
		NumericRangeList(void)
		{
			m_s.insert(NumericRange());
		}

		NumericRangeList(const NumericRangeList& n) : m_s(n.m_s)
		{
		}

		const NumericRangeList& operator =(const NumericRangeList& n)
		{
			m_s = n.m_s;
			return *this;
		}

		~NumericRangeList(void) {}

		const std::set<NumericRange> &getRangeSet(void) const { return m_s; }

		void clear()
		{
			m_s.clear();
			m_s.insert(NumericRange());
		}

		bool getRangeForNumber(N num, NumericRange &range) const
		{
			for (typename NRSet::const_iterator it = m_s.begin(); it != m_s.end() && it->m_from <= num; ++it)
				if (it->m_to >= num)
				{
					range = *it;
					return true;
				}
			return false;
		}

		bool contains(N num) const
		{
			for (typename NRSet::const_iterator it = m_s.begin(); it != m_s.end() && it->m_from <= num; ++it)
				if (it->m_to >= num)
					return true;
			return false;
		}

		NumericRangeList& invert()
		{
			N e = std::numeric_limits<N>::is_integer ? 1 : std::numeric_limits<N>::epsilon();
			N rngst = std::numeric_limits<N>::min();
			N rngen = std::numeric_limits<N>::max();
			NumericRangeList nwr;
			nwr.m_s.erase(nwr.m_s.begin());
			typename NRSet::iterator it = m_s.begin();
			for (; it != m_s.end(); it++)
			{
				if (it->m_from > rngst)
					nwr.m_s.insert(NumericRange(rngst, it->m_from - e));
				if (it->m_to < rngen)
					rngst = it->m_to + e;
			}
			if ((--it)->m_to < rngen)
				nwr.m_s.insert(NumericRange(rngst, rngen));
			m_s.swap(nwr.m_s);

			return *this;
		}

		NumericRangeList& operator += (const NumericRangeList& n)
		{
			m_s.insert(n.m_s.begin(), n.m_s.end());
			if (!m_s.empty())
			{
				typename NRSet::iterator it1 = m_s.begin(), it2 = m_s.begin();
				for (it2++; it2 != m_s.end();)
				{
					if (!it1->intersect(*it2).invalid()) // overlap
					{
						// We can do a const_cast here safely because
						// the we will never invalidate the sort order of the set
						NumericRange &l_nr = const_cast<NumericRange&>(*it1);
						l_nr.m_from = it1->m_from < it2->m_from ? it1->m_from : it2->m_from;
						l_nr.m_to = it1->m_to > it2->m_to ? it1->m_to : it2->m_to;
						typename NRSet::iterator ittmp = it2;
						++ittmp;
						m_s.erase(it2);
						if (ittmp ==m_s.end())
							break;
						it2 == ittmp;
						continue;
					}
					else
						it1++;
					it2++;
				}
			}
			return *this;
		}
		friend const NumericRangeList operator + (const NumericRangeList& l, const NumericRangeList& r){return NumericRangeList(l) += r;}

		bool addNum(N n)
		{
			N e = std::numeric_limits<N>::is_integer ? 1 : std::numeric_limits<N>::epsilon();
			bool ret = false;
			for (typename std::set<NumericRange>::iterator it = m_s.begin(); it != m_s.end(); ++it)
			{
				// We can do a const_cast here safely because
				// the we will never invalidate the sort order of the set
				NumericRange &l_nr = const_cast<NumericRange&>(*it);
				if (n <= l_nr.m_to)
				{
					if (n == l_nr.m_to)
					{
						if (l_nr.m_from == l_nr.m_to)
							m_s.erase(it);
						else
							l_nr.m_to = n - e; //
					}
					else if (n == l_nr.m_from)
						l_nr.m_from = n + e; //
					else if (n > l_nr.m_from)
					{
						N x = l_nr.m_from;
						l_nr.m_from = n + e; //
						m_s.insert(NumericRange(x, n - e));
					}
					else
						break;
					ret = true;
					break;
				}
			}
			return ret;
		}

		NumericRangeList& operator += (N n)
		{
			addNum(n);
			return *this;
		}

		bool removeNum(N n)
		{
			N e = std::numeric_limits<N>::is_integer ? 1 : std::numeric_limits<N>::epsilon();
			typename NRSet::iterator it = m_s.begin();
			typename NRSet::const_iterator prv = m_s.end();
			for (; it != m_s.end(); ++it)
			{
				if (n >= it->m_from && n <= it->m_to)
					return false;
				if (n < it->m_from)
					break;
				prv = it;
			}
			// We can do a const_cast here safely because
			// the we will never invalidate the sort order of the set
			NumericRange &l_nr = const_cast<NumericRange&>(*it);
			if (prv != m_s.end())
			{
				NumericRange &l_pnr = const_cast<NumericRange&>(*prv);
				if (n - e == l_pnr.m_to && n + e == l_nr.m_from)
				{
					l_pnr.m_to = l_nr.m_to;
					m_s.erase(it);
				}
				else if (n - e == l_pnr.m_to)
					l_pnr.m_to = n;
				else if (n + e == l_nr.m_from)
					l_nr.m_from = n;
				else
					m_s.insert(NumericRange(n,n));
			}
			else
			{
				if (n + e == l_nr.m_from)
					l_nr.m_from = n;
				else
					m_s.insert(NumericRange(n,n));
			}
			return true;
		}

		NumericRangeList& operator -= (N n)
		{
			removeNum(n);
			return *this;
		}

		NumericRangeList& operator += (const NumericRange &n)
		{
			NumericRangeList nrl;
			nrl.m_s.erase(nrl.m_s.begin());
			nrl.m_s.insert(n);

			return (invert() += nrl).invert();
		}

		NumericRangeList& operator -= (const NumericRange &n)
		{
			NumericRangeList nrl;
			nrl.m_s.erase(nrl.m_s.begin());
			nrl.m_s.insert(n);

			return *this += nrl;
		}
	};

private:
	NumericRangeList ranges;

public:

	NumericRangeHandler(void)
	{
	}

	NumericRangeHandler(const NumericRangeHandler& n) : ranges(n.ranges)
	{
	}

	const NumericRangeHandler& operator = (const NumericRangeHandler& n)
	{
		ranges = n.ranges;
		return *this;
	}

	~NumericRangeHandler(void)
	{
	}

	bool full()
	{
		return ranges.m_s.empty();
	}

	bool empty()
	{
		return ranges.m_s.size() == 1 && ranges.m_s.begin()->m_from == std::numeric_limits<N>::min() && ranges.m_s.begin()->m_to == std::numeric_limits<N>::max();
	}

	N getLowestUnused()
	{
		if (ranges.m_s.empty())
			return std::numeric_limits<N>::max();
		return ranges.m_s.begin()->m_from;
	}

	N addLowestUnused()
	{
		N ret = getLowestUnused();
		addNum(ret);
		return ret;
	}

	void clear()
	{
		ranges.clear();
	}

	N getSize() const
	{
		N ret = 0;
		N e = std::numeric_limits<N>::is_integer ? 1 : std::numeric_limits<N>::epsilon();

		NumericRangeList rl(getRanges());
		for (typename std::set<NumericRange>::const_iterator it = rl.m_s.begin(); it != rl.m_s.end(); ++it)
			ret += (it->m_to + e) - it->m_from;

		return ret;
	}

	typename NumericRangeHandler<N>::NumericRangeList getRanges() const
	{
		NumericRangeList ret(ranges);

		return ret.invert();
	}

	NumericRangeHandler<N>& operator += (const NumericRangeHandler<N>& n)
	{
		(ranges.invert() += n.getRanges()).invert();
		return *this;
	}

	NumericRangeHandler<N>& intersect(const NumericRangeHandler<N>& n)
	{
		ranges += n.ranges;
		return *this;
	}

	NumericRangeHandler<N>& operator -= (const NumericRangeHandler<N>& n)
	{
		ranges += n.getRanges();
		return *this;
	}

//	bool contains(const typename NumericRangeHandler<N>::NumericRange& n)
//	{
//	}

	bool contains(N n)
	{
		return !ranges.contains(n);
	}

	bool addNum(N n)
	{
		return ranges.addNum(n);
	}
	bool removeNum(N n)
	{
		return ranges.removeNum(n);
	}
	NumericRangeHandler<N>& operator += (N n)
	{
		ranges += n;
		return *this;
	}
	NumericRangeHandler<N>& operator -= (N n)
	{
		ranges -= n;
		return *this;
	}
	NumericRangeHandler<N>& operator += (const typename NumericRangeHandler<N>::NumericRange &n)
	{
		ranges += n;
		return *this;
	}
	NumericRangeHandler<N>& operator -= (const typename NumericRangeHandler<N>::NumericRange &n)
	{
		ranges -= n;
		return *this;
	}
};

template <class S, class Q> inline S& operator << (S& o, const NumericRangeHandler<Q>& n)
{
	typename NumericRangeHandler<Q>::NumericRangeList rl = n.getRanges();
	const std::set<typename NumericRangeHandler<Q>::NumericRange>& l = rl.getRangeSet();
	o << (unsigned int)l.size() << ' ';
	for (typename std::set<typename NumericRangeHandler<Q>::NumericRange>::const_iterator i = l.begin(); i != l.end(); o << *(i++));
	return o;
};

template <class S, class Q> inline S& operator >> (S& i, NumericRangeHandler<Q> &n)
{
	size_t cnt;
	i >> cnt;
	for (size_t x = 0; x < cnt; ++x)
	{
		typename NumericRangeHandler<Q>::NumericRange t;
		i >> t;
		n += t;
	}
	return i;
};

template <typename N> const N NumericRangeHandler<N>::MAX_N = std::numeric_limits<N>::max();
template <typename N> const N NumericRangeHandler<N>::MIN_N = std::numeric_limits<N>::min();


}

#endif //NMRH_NUMERIC_RANGE_HANDLER_H
