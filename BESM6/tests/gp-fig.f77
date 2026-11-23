      PROGRAM GPFIG
      INTEGER FRAME(64), ALL(64,14), X(14), Y(14), P(14)
      COMMON /PDEVXY/ FRAME
      EXTERNAL PDEV
      INTEGER I, J
      X(1)=3200
      X(2)=3060
      X(3)=2500
      X(4)=1940
      X(5)=1800
      X(6)=1940
      X(7)=2500
      X(8)=3060
      X(9)=3200
      X(10)=500
      X(11)=4500
      X(12)=4500
      X(13)=500
      X(14)=500
      Y(1)=3750
      Y(2)=4760
      Y(3)=5250
      Y(4)=4760
      Y(5)=3750
      Y(6)=2740
      Y(7)=2250
      Y(8)=2740
      Y(9)=3750
      Y(10)=500
      Y(11)=500
      Y(12)=7000
      Y(13)=7000
      Y(14)=500
      P(1)=0
      P(2)=1
      P(3)=1
      P(4)=1
      P(5)=1
      P(6)=1
      P(7)=1
      P(8)=1
      P(9)=1
      P(10)=0
      P(11)=1
      P(12)=1
      P(13)=1
      P(14)=1
      DO 20 J = 1, 14
         DO 10 I = 1, 64
   10    FRAME(I) = 0
         FRAME(1) = X(J)
         FRAME(2) = Y(J)
         FRAME(3) = P(J)
         DO 15 I = 1, 64
   15    ALL(I,J) = FRAME(I)
   20 CONTINUE
      PRINT 900
  900 FORMAT(' 14 12 3200 500 777 12345')
      CALL PDEV(FRAME(1))
      END
