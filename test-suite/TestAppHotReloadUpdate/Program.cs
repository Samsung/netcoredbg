using System;using System.Diagnostics;using System.Threading;using System.Reflection.Metadata;[assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.HotReload1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program.HotReload))][assembly: MetadataUpdateHandler(typeof(TestAppHotReloadUpdate.Program.HotReload.HotReloadNested))]
                namespace TestAppHotReloadUpdate
                {
                    public class Program
                    {
                        static public int i_test1 = 0;
                        static public int i_test2 = 0;
                        static public int i_test3 = 0;
                        static public int i_test4 = 0;
                        static public int i_test5 = 0;
                        static public int i_test6 = 0;

                        static void Main(string[] args)
                        {
                            Console.WriteLine("Hello World!");
                            HotReloadTest();
                        }
                        static void HotReloadTest()
                        {
                            Console.WriteLine("Initial string.");
                        }

        internal static class HotReload
        {
            public static void ClearCache(Type[]? changedTypes)
            {
                Console.WriteLine("ClearCache2");
            }
            internal static class HotReloadNested
            {
                public static void UpdateApplication(Type[]? changedTypes)
                {
                    Console.WriteLine("UpdateApplication3");
                }
            }
        }
                    }
    internal static class HotReload1111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111
    {
        public static void ClearCache(Type[]? changedTypes)
        {
            Console.WriteLine("ClearCache1");
        }

        public static void UpdateApplication(Type[]? changedTypes)
        {
            Console.WriteLine("UpdateApplication1");
        }
    }
                    }
