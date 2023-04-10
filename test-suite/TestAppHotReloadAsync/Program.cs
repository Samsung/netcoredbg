using System;
using System.Threading.Tasks;
                namespace TestAppHotReloadAsync
                {
                    class Program
                    {
                        static async Task Main(string[] args)
                        {   System.Threading.Thread.Sleep(500);
                            Console.WriteLine("Hello World!");
                            await HotReloadTestAsync();
                        }
                        static async Task HotReloadTestAsync()
                        {
                            Console.WriteLine("Initial string.");
                            await Task.Delay(100);
                        }
                    }
                }
