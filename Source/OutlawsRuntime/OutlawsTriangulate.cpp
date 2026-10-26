#include "OutlawsTriangulate.h"
#include "OutlawsPolygon.h"

/*
 * A port of mapbox earcut (earcut.js 2.2.4, ISC licence, Copyright (c) 2016 Mapbox) without
 * the z-order hash (sectors are small, and the hash only speeds up the same ear test).  The
 * exporter triangulates with the same algorithm (mapbox_earcut), so floors and ceilings match.
 */
namespace
{
	struct FNode
	{
		int32 I = 0;          // index into the input points
		double X = 0.0;
		double Y = 0.0;
		int32 Prev = -1;
		int32 Next = -1;
		bool bSteiner = false;
	};

	class FEarcut
	{
	public:
		FEarcut(const TArray<FVector2D>& InP, TArray<int32>& InTris) : P(InP), Tris(InTris) {}

		void Run(const TArray<int32>& RingEnds)
		{
			const int32 OuterEnd = RingEnds[0];
			int32 Outer = LinkedList(0, OuterEnd, true);
			if (Outer < 0 || Nodes[Outer].Next == Nodes[Outer].Prev)
			{
				return;
			}
			if (RingEnds.Num() > 1)
			{
				Outer = EliminateHoles(RingEnds, Outer);
			}
			EarcutLinked(Outer, 0);
		}

		int32 Fallbacks = 0;

	private:
		const TArray<FVector2D>& P;
		TArray<int32>& Tris;
		TArray<FNode> Nodes;

		FNode& N(int32 i) { return Nodes[i]; }

		double SignedArea(int32 Start, int32 End) const
		{
			double Sum = 0.0;
			for (int32 i = Start, j = End - 1; i < End; j = i++)
			{
				Sum += (P[j].X - P[i].X) * (P[i].Y + P[j].Y);
			}
			return Sum;
		}

		int32 InsertNode(int32 i, int32 Last)
		{
			FNode Nd;
			Nd.I = i;
			Nd.X = P[i].X;
			Nd.Y = P[i].Y;
			const int32 Idx = Nodes.Add(Nd);
			if (Last < 0)
			{
				N(Idx).Prev = Idx;
				N(Idx).Next = Idx;
			}
			else
			{
				N(Idx).Next = N(Last).Next;
				N(Idx).Prev = Last;
				N(N(Last).Next).Prev = Idx;
				N(Last).Next = Idx;
			}
			return Idx;
		}

		void RemoveNode(int32 p)
		{
			N(N(p).Next).Prev = N(p).Prev;
			N(N(p).Prev).Next = N(p).Next;
		}

		int32 LinkedList(int32 Start, int32 End, bool bClockwise)
		{
			int32 Last = -1;
			if (bClockwise == (SignedArea(Start, End) > 0.0))
			{
				for (int32 i = Start; i < End; ++i) Last = InsertNode(i, Last);
			}
			else
			{
				for (int32 i = End - 1; i >= Start; --i) Last = InsertNode(i, Last);
			}
			if (Last >= 0 && Equals(Last, N(Last).Next))
			{
				RemoveNode(Last);
				Last = N(Last).Next;
			}
			return Last;
		}

		bool Equals(int32 a, int32 b) { return N(a).X == N(b).X && N(a).Y == N(b).Y; }

		double Area(int32 p, int32 q, int32 r)
		{
			return (N(q).Y - N(p).Y) * (N(r).X - N(q).X) - (N(q).X - N(p).X) * (N(r).Y - N(q).Y);
		}

		static bool PointInTriangle(double ax, double ay, double bx, double by, double cx, double cy, double px, double py)
		{
			return (cx - px) * (ay - py) >= (ax - px) * (cy - py) &&
				(ax - px) * (by - py) >= (bx - px) * (ay - py) &&
				(bx - px) * (cy - py) >= (cx - px) * (by - py);
		}

		int32 FilterPoints(int32 Start, int32 End)
		{
			if (Start < 0) return Start;
			if (End < 0) End = Start;
			int32 p = Start;
			bool bAgain;
			do
			{
				bAgain = false;
				if (!N(p).bSteiner && (Equals(p, N(p).Next) || Area(N(p).Prev, p, N(p).Next) == 0.0))
				{
					RemoveNode(p);
					p = End = N(p).Prev;
					if (p == N(p).Next) break;
					bAgain = true;
				}
				else
				{
					p = N(p).Next;
				}
			} while (bAgain || p != End);
			return End;
		}

		void EarcutLinked(int32 Ear, int32 Pass)
		{
			if (Ear < 0) return;
			int32 Stop = Ear;
			while (N(Ear).Prev != N(Ear).Next)
			{
				const int32 Prev = N(Ear).Prev;
				const int32 Next = N(Ear).Next;
				if (IsEar(Ear))
				{
					Tris.Add(N(Prev).I);
					Tris.Add(N(Ear).I);
					Tris.Add(N(Next).I);
					RemoveNode(Ear);
					Ear = N(Next).Next;      // skipping the next vertex gives fewer slivers
					Stop = N(Next).Next;
					continue;
				}
				Ear = Next;
				if (Ear == Stop)
				{
					if (Pass == 0)
					{
						EarcutLinked(FilterPoints(Ear, -1), 1);
					}
					else if (Pass == 1)
					{
						++Fallbacks;
						const int32 E = CureLocalIntersections(FilterPoints(Ear, -1));
						EarcutLinked(E, 2);
					}
					else if (Pass == 2)
					{
						SplitEarcut(Ear);
					}
					break;
				}
			}
		}

		bool IsEar(int32 Ear)
		{
			const int32 a = N(Ear).Prev, b = Ear, c = N(Ear).Next;
			if (Area(a, b, c) >= 0.0) return false;   // reflex
			const double ax = N(a).X, bx = N(b).X, cx = N(c).X, ay = N(a).Y, by = N(b).Y, cy = N(c).Y;
			const double x0 = FMath::Min3(ax, bx, cx), y0 = FMath::Min3(ay, by, cy);
			const double x1 = FMath::Max3(ax, bx, cx), y1 = FMath::Max3(ay, by, cy);
			int32 p = N(c).Next;
			while (p != a)
			{
				const FNode& Q = N(p);
				if (Q.X >= x0 && Q.X <= x1 && Q.Y >= y0 && Q.Y <= y1 &&
					PointInTriangle(ax, ay, bx, by, cx, cy, Q.X, Q.Y) &&
					Area(Q.Prev, p, Q.Next) >= 0.0)
				{
					return false;
				}
				p = N(p).Next;
			}
			return true;
		}

		int32 CureLocalIntersections(int32 Start)
		{
			int32 p = Start;
			do
			{
				const int32 a = N(p).Prev, b = N(N(p).Next).Next;
				if (!Equals(a, b) && Intersects(a, p, N(p).Next, b) && LocallyInside(a, b) && LocallyInside(b, a))
				{
					Tris.Add(N(a).I);
					Tris.Add(N(p).I);
					Tris.Add(N(b).I);
					const int32 PN = N(p).Next;
					RemoveNode(p);
					RemoveNode(PN);
					p = Start = b;
				}
				p = N(p).Next;
			} while (p != Start);
			return FilterPoints(p, -1);
		}

		void SplitEarcut(int32 Start)
		{
			int32 a = Start;
			do
			{
				int32 b = N(N(a).Next).Next;
				while (b != N(a).Prev)
				{
					if (N(a).I != N(b).I && IsValidDiagonal(a, b))
					{
						int32 c = SplitPolygon(a, b);
						a = FilterPoints(a, N(a).Next);
						c = FilterPoints(c, N(c).Next);
						EarcutLinked(a, 0);
						EarcutLinked(c, 0);
						return;
					}
					b = N(b).Next;
				}
				a = N(a).Next;
			} while (a != Start);
		}

		int32 EliminateHoles(const TArray<int32>& RingEnds, int32 Outer)
		{
			TArray<int32> Queue;
			for (int32 r = 1; r < RingEnds.Num(); ++r)
			{
				const int32 List = LinkedList(RingEnds[r - 1], RingEnds[r], false);
				if (List < 0) continue;
				if (List == N(List).Next) N(List).bSteiner = true;
				Queue.Add(GetLeftmost(List));
			}
			Queue.StableSort([this](int32 A, int32 B) { return Nodes[A].X < Nodes[B].X; });
			for (int32 h : Queue)
			{
				Outer = EliminateHole(h, Outer);
			}
			return Outer;
		}

		int32 EliminateHole(int32 Hole, int32 Outer)
		{
			const int32 Bridge = FindHoleBridge(Hole, Outer);
			if (Bridge < 0)
			{
				return Outer;
			}
			const int32 BridgeReverse = SplitPolygon(Bridge, Hole);
			FilterPoints(BridgeReverse, N(BridgeReverse).Next);
			return FilterPoints(Bridge, N(Bridge).Next);
		}

		int32 FindHoleBridge(int32 Hole, int32 Outer)
		{
			int32 p = Outer;
			const double hx = N(Hole).X, hy = N(Hole).Y;
			double qx = -TNumericLimits<double>::Max();
			int32 m = -1;
			do
			{
				const FNode& A = N(p);
				const FNode& B = N(A.Next);
				if (hy <= A.Y && hy >= B.Y && B.Y != A.Y)
				{
					const double x = A.X + (hy - A.Y) * (B.X - A.X) / (B.Y - A.Y);
					if (x <= hx && x > qx)
					{
						qx = x;
						m = A.X < B.X ? p : A.Next;
						if (x == hx) return m;   // hole touches the outer segment
					}
				}
				p = N(p).Next;
			} while (p != Outer);
			if (m < 0) return -1;

			const int32 Stop = m;
			const double mx = N(m).X, my = N(m).Y;
			double TanMin = TNumericLimits<double>::Max();
			p = m;
			do
			{
				const double px = N(p).X, py = N(p).Y;
				if (hx >= px && px >= mx && hx != px &&
					PointInTriangle(hy < my ? hx : qx, hy, mx, my, hy < my ? qx : hx, hy, px, py))
				{
					const double Tan = FMath::Abs(hy - py) / (hx - px);
					if (LocallyInside(p, Hole) &&
						(Tan < TanMin || (Tan == TanMin && (px > N(m).X || (px == N(m).X && SectorContainsSector(m, p))))))
					{
						m = p;
						TanMin = Tan;
					}
				}
				p = N(p).Next;
			} while (p != Stop);
			return m;
		}

		bool SectorContainsSector(int32 m, int32 p)
		{
			return Area(N(m).Prev, m, N(p).Prev) < 0.0 && Area(N(p).Next, m, N(m).Next) < 0.0;
		}

		int32 GetLeftmost(int32 Start)
		{
			int32 p = Start, Leftmost = Start;
			do
			{
				if (N(p).X < N(Leftmost).X || (N(p).X == N(Leftmost).X && N(p).Y < N(Leftmost).Y)) Leftmost = p;
				p = N(p).Next;
			} while (p != Start);
			return Leftmost;
		}

		bool IsValidDiagonal(int32 a, int32 b)
		{
			return N(N(a).Next).I != N(b).I && N(N(a).Prev).I != N(b).I && !IntersectsPolygon(a, b) &&
				((LocallyInside(a, b) && LocallyInside(b, a) && MiddleInside(a, b) &&
					(Area(N(a).Prev, a, N(b).Prev) != 0.0 || Area(a, N(b).Prev, b) != 0.0)) ||
				(Equals(a, b) && Area(N(a).Prev, a, N(a).Next) > 0.0 && Area(N(b).Prev, b, N(b).Next) > 0.0));
		}

		static int32 Sign(double v) { return v > 0.0 ? 1 : v < 0.0 ? -1 : 0; }

		bool OnSegment(int32 p, int32 q, int32 r)
		{
			return N(q).X <= FMath::Max(N(p).X, N(r).X) && N(q).X >= FMath::Min(N(p).X, N(r).X) &&
				N(q).Y <= FMath::Max(N(p).Y, N(r).Y) && N(q).Y >= FMath::Min(N(p).Y, N(r).Y);
		}

		bool Intersects(int32 p1, int32 q1, int32 p2, int32 q2)
		{
			const int32 o1 = Sign(Area(p1, q1, p2));
			const int32 o2 = Sign(Area(p1, q1, q2));
			const int32 o3 = Sign(Area(p2, q2, p1));
			const int32 o4 = Sign(Area(p2, q2, q1));
			if (o1 != o2 && o3 != o4) return true;
			if (o1 == 0 && OnSegment(p1, p2, q1)) return true;
			if (o2 == 0 && OnSegment(p1, q2, q1)) return true;
			if (o3 == 0 && OnSegment(p2, p1, q2)) return true;
			if (o4 == 0 && OnSegment(p2, q1, q2)) return true;
			return false;
		}

		bool IntersectsPolygon(int32 a, int32 b)
		{
			int32 p = a;
			do
			{
				const int32 pn = N(p).Next;
				if (N(p).I != N(a).I && N(pn).I != N(a).I && N(p).I != N(b).I && N(pn).I != N(b).I &&
					Intersects(p, pn, a, b))
				{
					return true;
				}
				p = pn;
			} while (p != a);
			return false;
		}

		bool LocallyInside(int32 a, int32 b)
		{
			return Area(N(a).Prev, a, N(a).Next) < 0.0
				? Area(a, b, N(a).Next) >= 0.0 && Area(a, N(a).Prev, b) >= 0.0
				: Area(a, b, N(a).Prev) < 0.0 || Area(a, N(a).Next, b) < 0.0;
		}

		bool MiddleInside(int32 a, int32 b)
		{
			int32 p = a;
			bool bInside = false;
			const double px = (N(a).X + N(b).X) / 2.0, py = (N(a).Y + N(b).Y) / 2.0;
			do
			{
				const FNode& A = N(p);
				const FNode& B = N(A.Next);
				if (OutlawsPolygon::Crosses(FVector2D(A.X, A.Y), FVector2D(B.X, B.Y), px, py))      // (B.Y != A.Y: implied)
				{
					bInside = !bInside;
				}
				p = A.Next;
			} while (p != a);
			return bInside;
		}

		int32 SplitPolygon(int32 a, int32 b)
		{
			FNode A2, B2;
			A2.I = N(a).I; A2.X = N(a).X; A2.Y = N(a).Y;
			B2.I = N(b).I; B2.X = N(b).X; B2.Y = N(b).Y;
			const int32 a2 = Nodes.Add(A2);
			const int32 b2 = Nodes.Add(B2);
			const int32 an = N(a).Next;
			const int32 bp = N(b).Prev;
			N(a).Next = b;
			N(b).Prev = a;
			N(a2).Next = an;
			N(an).Prev = a2;
			N(b2).Next = a2;
			N(a2).Prev = b2;
			N(bp).Next = b2;
			N(b2).Prev = bp;
			return b2;
		}
	};
}

int32 OutlawsTriangulate::Earcut(const TArray<FVector2D>& Points, const TArray<int32>& RingEnds, TArray<int32>& OutTriangles)
{
	OutTriangles.Reset();
	if (RingEnds.Num() == 0 || Points.Num() < 3 || RingEnds[0] < 3)
	{
		return 0;
	}
	FEarcut E(Points, OutTriangles);
	E.Run(RingEnds);
	return E.Fallbacks;
}

double OutlawsTriangulate::TrianglesArea(const TArray<FVector2D>& Points, const TArray<int32>& Triangles)
{
	double A = 0.0;
	for (int32 t = 0; t + 2 < Triangles.Num(); t += 3)
	{
		const FVector2D& P = Points[Triangles[t]];
		const FVector2D& Q = Points[Triangles[t + 1]];
		const FVector2D& R = Points[Triangles[t + 2]];
		A += FMath::Abs((Q.X - P.X) * (R.Y - P.Y) - (Q.Y - P.Y) * (R.X - P.X)) * 0.5;
	}
	return A;
}
