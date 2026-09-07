        // The rejection this file asked for, and it needed no vector unit.
        // Substituting b and c into d = b*b - q2*c leaves a quadratic in dx
        // whose three coefficients are constant along the row, so the columns
        // that can hit are one interval, found with a single sqrt per petal-row
        // instead of a discriminant per pixel. Measured on the host, 51-72% of
        // ellipsoid visits were d<0 misses; those columns now cost nothing.
        //
        // This is a narrowing of the loop bounds, not a change to the test: the
        // per-pixel d<0 check below still runs, and the interval is widened by
        // one column each side so that float rounding here can never exclude a
        // column the exact test would have kept. The output is bit-identical.
        //
        // Bells keep the full span. bell_hit intersects six conical bands and
        // its miss set is not one interval in dx; a bounding form for it is
        // worth measuring separately, and 59-64% of its visits also miss.
        int x0=p->xmin,x1=p->xmax;
        if(!p->shape) {
            float A=p->q[4]*p->q[4]-p->q[2]*p->q[0];
            float B=dy*(p->q[4]*p->q[5]-p->q[2]*p->q[3]);
            float C=dy*dy*(p->q[5]*p->q[5]-p->q[2]*p->q[1])+p->q[2];
            // A<0 is the projected ellipse. A>=0 would make the hit set
            // unbounded or degenerate, and then the full span is kept.
            if(A<0) {
                float disc=B*B-A*C;
                if(disc<0)continue;
                float sd=sqrtf(disc),r1=(-B+sd)/A,r2=(-B-sd)/A;
                float lo=r1<r2?r1:r2,hi=r1<r2?r2:r1;
                int cl=(int)floorf((lo+p->c.x)*SCALE+179.5f)-1;
                int ch=(int)ceilf ((hi+p->c.x)*SCALE+179.5f)+1;
                if(cl>x0)x0=cl;
                if(ch<x1)x1=ch;
            }
        }
        for(int x=x0;x<=x1;x++) {
