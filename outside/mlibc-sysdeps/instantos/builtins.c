float _Complex __mulsc3(float a, float b, float c, float d) {
  return (a * c - b * d) + (a * d + b * c) * 1.0fi;
}

double _Complex __muldc3(double a, double b, double c, double d) {
  return (a * c - b * d) + (a * d + b * c) * 1.0i;
}

long double _Complex __mulxc3(long double a, long double b, long double c, long double d) {
  return (a * c - b * d) + (a * d + b * c) * 1.0Li;
}
