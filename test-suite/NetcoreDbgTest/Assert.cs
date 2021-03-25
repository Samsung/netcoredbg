namespace NetcoreDbgTest
{
    public static class Assert
    {
        public static void Equal<T>(T expected, T actual, string stacktrace)
        {
            bool areEqual;

            if (expected == null || actual == null)
                areEqual = (expected == null && actual == null);
            else
                areEqual = expected.Equals(actual);

            if (!areEqual)
                throw new ResultEqualException(expected, actual, stacktrace);
        }

        public static void NotEqual<T>(T expected, T actual, string stacktrace)
        {
            bool areEqual;

            if (expected == null || actual == null)
                areEqual = (expected == null && actual == null);
            else
                areEqual = expected.Equals(actual);

            if (areEqual)
                throw new ResultNotEqualException(expected, actual, stacktrace);
        }

        public static void True(bool isTrue, string stacktrace)
        {
            if (!isTrue)
                throw new ResultTrueException("true", "false", stacktrace);
        }

        public static void False(bool isTrue, string stacktrace)
        {
            if (isTrue)
                throw new ResultFalseException("false", "true", stacktrace);
        }

        public static void NotNull<T>(T expected, string stacktrace)
        {
            if (expected == null)
                throw new ResultNotNullException(stacktrace);
        }
    }
}
